//
//  Scheduler.cpp
//  CloudSim
//
//  Created by ELMOOTAZBELLAH ELNOZAHY on 10/20/24.
//

#include "Scheduler.hpp"
#include <algorithm>
#include <limits>

static int finished_tasks = 0;
static bool migrating = false;
static unsigned active_machines = 16;
vector<MachineId_t> machines_running;
vector<MachineId_t> machines_standby;
vector<MachineId_t> machines_sleeping;
vector<MachineId_t> machines_transitioning;
static Scheduler* scheduler = nullptr;

unordered_map<MachineId_t, vector<TaskId_t>> pending_tasks_for_machine;
vector<TaskId_t> general_pending_tasks;

const VMId_t VM_NOT_FOUND = numeric_limits<VMId_t>::max();

void Scheduler::Init() {
    SimOutput("Scheduler::Init(): Total number of machines is " + to_string(Machine_GetTotal()), 3);
    SimOutput("Scheduler::Init(): Initializing scheduler", 1);
    scheduler = this;
    
    // set one machine to running
    vms.push_back(VM_Create(LINUX, X86));
    machines_running.push_back(MachineId_t(0));
    VM_Attach(vms[0], machines_running[0]);

    // set one machine to standby
    Machine_SetState(MachineId_t(1), S2);
    machines_standby.push_back(MachineId_t(1));

    // set rest of machines to sleep
    unsigned total_machines = Machine_GetTotal();
    for (unsigned i = 2; i < total_machines; i++) {
        MachineId_t machine = MachineId_t(i);
        Machine_SetState(machine, S5);  // put the machines into sleep
        machines_sleeping.push_back(machine);
    }

    cout << "Machines running: " << machines_running.size() << endl;
    cout << "Machines standby: " << machines_standby.size() << endl;
    cout << "Machines sleeping: " << machines_sleeping.size() << endl;

    bool dynamic = false;
    if(dynamic)
        for(unsigned i = 0; i<4 ; i++)
            for(unsigned j = 0; j < 8; j++)
                Machine_SetCorePerformance(MachineId_t(0), j, P3);
    // Turn off the ARM machines
    // for(unsigned i = 24; i < Machine_GetTotal(); i++)
    //     Machine_SetState(MachineId_t(i), S5);

    //SimOutput("Scheduler::Init(): VM ids are " + to_string(vms[0]) + " ahd " + to_string(vms[1]), 3);
}

void Scheduler::MigrationComplete(Time_t time, VMId_t vm_id) {
    // Update your data structure. The VM now can receive new tasks
}

static double GetCPUUtilWithNewTask(MachineInfo_t machine_info, vector<VMId_t>& vms, TaskInfo_t new_task_info) {
    double total_instr_remaining = 0;

    for (VMId_t vm_id : vms) {
        VMInfo_t vm_info = VM_GetInfo(vm_id);
        if (vm_info.machine_id != machine_info.machine_id)
            continue;

        for (TaskId_t t_id : vm_info.active_tasks) {
            TaskInfo_t t_info = GetTaskInfo(t_id);
            total_instr_remaining += t_info.remaining_instructions;
        }
    }

    total_instr_remaining += new_task_info.remaining_instructions;

    // Convert instructions to a fraction of machine capacity per second
    double machine_capacity = machine_info.num_cpus * machine_info.performance[S0] * 1e6;
    return total_instr_remaining / machine_capacity;
}

static double GetMemUtil(MachineInfo_t machine_info, TaskInfo_t t_info) {
    // current memory used plus memory requested by new task
    double mem_util = double(machine_info.memory_used + t_info.required_memory) / machine_info.memory_size;
    return mem_util;
}

static bool IsMachineInList(MachineId_t machine_id, vector<MachineId_t> &vector) {
    auto it = find(vector.begin(), vector.end(), machine_id);
    return it != vector.end();  
}

static void RemoveMachineFromList(MachineId_t machine_id, vector<MachineId_t> &vector) {
    auto it = find(vector.begin(), vector.end(), machine_id);
    if (it != vector.end()) {
        vector.erase(it);
    }
}

static void RemoveTaskFromList(TaskId_t task_id, vector<TaskId_t> &vector) {
    auto it = find(vector.begin(), vector.end(), task_id);
    if (it != vector.end()) {
        vector.erase(it);
    }
}

static void RemoveVMFromList(VMId_t vm_id, vector<TaskId_t> &vector) {
    auto it = find(vector.begin(), vector.end(), vm_id);
    if (it != vector.end()) {
        vector.erase(it);
    }
}


static VMId_t GetVMForMachine(vector<VMId_t> &vms, MachineId_t machine, TaskInfo_t task_info) {
    MachineInfo_t mach_info = Machine_GetInfo(machine);
    for (VMId_t vm_id : vms) {
        VMInfo_t vm_info = VM_GetInfo(vm_id);
        if (vm_info.machine_id == machine && vm_info.vm_type == task_info.required_vm) {
            return vm_id;
        }
    }
    return VM_NOT_FOUND;
}

static bool PredictMachineIsCapable(TaskInfo_t task_info, MachineId_t machine_id, vector<VMId_t> &vms) {
    MachineInfo_t machine_info = Machine_GetInfo(machine_id);
    double total_instr_remaining = 0;
    double total_memory_used = 0;

    for (TaskId_t t_id : pending_tasks_for_machine[machine_id]) {
        TaskInfo_t t_info = GetTaskInfo(t_id);
        total_instr_remaining += t_info.remaining_instructions;
        total_memory_used += t_info.required_memory;
    }

    total_instr_remaining += task_info.remaining_instructions;
    total_memory_used += task_info.required_memory;

    double machine_capacity = machine_info.num_cpus * machine_info.performance[S0] * 1e6;
    bool CPUOK = GetCPUUtilWithNewTask(machine_info, vms, task_info) + (total_instr_remaining / machine_capacity) < 1.0;

    bool memoryOK = total_memory_used < machine_info.memory_size;

    return CPUOK && memoryOK;

}

// Checks if machine has 1) correct cpu 2) cpu won't be overloaded 3) memory won't be overloaded
static bool IsMachineCapable(TaskInfo_t task_info, MachineId_t machine_id, vector<VMId_t> &vms) {
    MachineInfo_t machine_info = Machine_GetInfo(machine_id);
    bool correct_cpu = task_info.required_cpu == machine_info.cpu;
    bool cpu_not_overloaded = (PredictMachineIsCapable(task_info, machine_id, vms));

    bool memory_not_overloaded = GetMemUtil(machine_info, task_info);

    return correct_cpu && cpu_not_overloaded && memory_not_overloaded;
}

void WakeUpMachine(TaskId_t task_id, vector<VMId_t> &vms) {
    bool task_assigned = false;
    TaskInfo_t task_info = GetTaskInfo(task_id);

    for (auto machine_id : machines_standby) {
        MachineInfo_t machine_info = Machine_GetInfo(machine_id);
        bool correct_cpu = task_info.required_cpu == machine_info.cpu;
        if (correct_cpu && !IsMachineInList(machine_id, machines_transitioning) && PredictMachineIsCapable(task_info, machine_id, vms)) {
            // machine is not transitioning but is capable. Start transition and queue task
            machines_transitioning.push_back(machine_id);
            machines_running.push_back(machine_id);
            RemoveMachineFromList(machine_id, machines_standby);
            Machine_SetState(machine_id, S0);
            pending_tasks_for_machine[machine_id].push_back(task_id);
            task_assigned = true;
            cout << "Starting to transition machine " << machine_id << " to state S0" << endl;
            cout << "standby size: " << machines_standby.size() << endl;
            break;
        }
    }

    for (auto machine : machines_sleeping) {
        MachineInfo_t machine_info = Machine_GetInfo(machine);
        if (machine_info.cpu == task_info.required_cpu) {
            if (!task_assigned) {
                pending_tasks_for_machine[machine].push_back(task_id);
            }
            cout << "Adding machine " << machine << " to standby list" << endl;
            machines_transitioning.push_back(machine);
            machines_standby.push_back(machine);
            RemoveMachineFromList(machine, machines_sleeping);
            Machine_SetState(machine, S2);
            return;
        }
    }

    if (!task_assigned) {
        cout << "Pushing task " << task_id << " to general pending list" << endl;
        general_pending_tasks.push_back(task_id);
        cout << "General pending list size: " << general_pending_tasks.size() << endl;
    }
}

void Scheduler::NewTask(Time_t now, TaskId_t task_id) {
    TaskInfo_t task_info = GetTaskInfo(task_id);
    CPUType_t req_CPU = RequiredCPUType(task_id);
    VMType_t req_VM = RequiredVMType(task_id);
    unsigned req_mem = GetTaskMemory(task_id);
    cout << "Task " << task_id << " coming in!" << endl;;

    for (auto machine : machines_running) {
        MachineInfo_t machine_info = Machine_GetInfo(machine);
        VMId_t vm_id = GetVMForMachine(vms, machine, task_info);
        
        // check correct cpu type and will not be overloaded with new task and has correct vm type
        if (IsMachineCapable(task_info, machine, vms)) {
            // if machine is fully S0 (not transitioning) then attach task
            if (!IsMachineInList(machine, machines_transitioning)) {
                if (vm_id != VM_NOT_FOUND) {
                    VMInfo_t vm_info = VM_GetInfo(vm_id);
                    VM_AddTask(vm_id, task_id, HIGH_PRIORITY);

                    cout << "Machine " << machine << " utilization: " << GetCPUUtilWithNewTask(machine_info, vms, task_info) << endl;;
                    cout << "Task " << task_id << " added to machine " << machine << endl;
                } else {
                    pending_tasks_for_machine[machine].push_back(task_id);
                }
            } else {
                pending_tasks_for_machine[machine].push_back(task_id);
                cout << "Adding task " << task_id << " to pending list of machine " << machine << endl;
            }
            return;
        }
    }

    //no capable machine found in running; wake up a machine
    WakeUpMachine(task_id, vms);
}

static void ExecuteTasks(MachineId_t machine, vector<VMId_t> &vms) {
    auto &pending = pending_tasks_for_machine[machine];
    // Process until the pending list is empty
    while (!pending.empty()) {
        TaskId_t task = pending.front();
        RemoveTaskFromList(task, pending);

        TaskInfo_t task_info = GetTaskInfo(task);
        VMId_t capable_vm = GetVMForMachine(vms, machine, task_info);
        if (capable_vm == VM_NOT_FOUND) {
            VMId_t new_vm = VM_Create(task_info.required_vm, task_info.required_cpu);
            vms.push_back(new_vm);
            VM_Attach(new_vm, machine);
            VM_AddTask(new_vm, task, HIGH_PRIORITY);
        } else {
            VM_AddTask(capable_vm, task, HIGH_PRIORITY);
        }
        cout << "In execute tasks: Attaching task " << task << " to machine " << machine << endl;
    }
}


void Scheduler::PeriodicCheck(Time_t now) {
    // This method should be called from SchedulerCheck()
    // SchedulerCheck is called periodically by the simulator to allow you to monitor, make decisions, adjustments, etc.
    // Unlike the other invocations of the scheduler, this one doesn't report any specific event
    // Recommendation: Take advantage of this function to do some monitoring and adjustments as necessary  
    for (auto machine : machines_running) {
        if (IsMachineInList(machine, machines_transitioning)) {
            continue;
        }
        
        ExecuteTasks(machine, vms);
        MachineInfo_t machine_info = Machine_GetInfo(machine);
        for (auto it = general_pending_tasks.begin(); it != general_pending_tasks.end(); ) {
            auto task_id = *it;
            TaskInfo_t task_info = GetTaskInfo(task_id);

            if (IsMachineCapable(task_info, machine, vms)) {
                // RemoveTaskFromList(task_id, general_pending_tasks);
                it = general_pending_tasks.erase(it);
                cout << "Attaching task " << task_id << " to machine " << machine << endl;
                VM_AddTask(GetVMForMachine(vms, machine, task_info), task_id, HIGH_PRIORITY);
            } else {
                it++;
            }

            for (auto vm : vms) {
                VMInfo_t vm_info = VM_GetInfo(vm);
                if (vm_info.machine_id == machine && vm_info.active_tasks.size() == 0) {
                    RemoveVMFromList(vm, vms);
                    VM_Shutdown(vm);
                }
            }
        }

        if (machine_info.active_tasks == 0) {
            machines_standby.push_back(machine);
            machines_transitioning.push_back(machine);
            RemoveMachineFromList(machine, machines_running);

            for (auto vm : scheduler->vms) {
                VMInfo_t vm_info = VM_GetInfo(vm);
                if (vm_info.machine_id == machine && vm_info.active_tasks.size() == 0) {
                    RemoveVMFromList(vm, scheduler->vms);
                    VM_Shutdown(vm);
                }
            }
            Machine_SetState(machine, S2);
        }
    }

    for (auto machine : machines_standby) {
        MachineInfo_t machine_info = Machine_GetInfo(machine);
        if (pending_tasks_for_machine[machine].size() == 0) {
            machines_sleeping.push_back(machine);
            machines_transitioning.push_back(machine);
            RemoveMachineFromList(machine, machines_standby);
            
            
            Machine_SetState(machine, S5);
        }
    }
}



void Scheduler::Shutdown(Time_t time) {
    // Do your final reporting and bookkeeping here.
    // Report about the total energy consumed
    // Report about the SLA compliance
    // Shutdown everything to be tidy :-)
    for(auto & vm: vms) {
        VM_Shutdown(vm);
    }
    SimOutput("SimulationComplete(): Finished!", 4);
    SimOutput("SimulationComplete(): Time is " + to_string(time), 4);
}

void Scheduler::TaskComplete(Time_t now, TaskId_t task_id) {
    // Do any bookkeeping necessary for the data structures
    // Decide if a machine is to be turned off, slowed down, or VMs to be migrated according to your policy
    // This is an opportunity to make any adjustments to optimize performance/energy
    SimOutput("Scheduler::TaskComplete(): Task " + to_string(task_id) + " is complete at " + to_string(now), 4);
}

// Public interface below

static Scheduler Scheduler;

void InitScheduler() {
    SimOutput("InitScheduler(): Initializing scheduler", 4);
    Scheduler.Init();
}

void HandleNewTask(Time_t time, TaskId_t task_id) {
    SimOutput("HandleNewTask(): Received new task " + to_string(task_id) + " at time " + to_string(time), 4);
    Scheduler.NewTask(time, task_id);
}

void HandleTaskCompletion(Time_t time, TaskId_t task_id) {
    SimOutput("HandleTaskCompletion(): Task " + to_string(task_id) + " completed at time " + to_string(time), 4);
    Scheduler.TaskComplete(time, task_id);
}

void MemoryWarning(Time_t time, MachineId_t machine_id) {
    // The simulator is alerting you that machine identified by machine_id is overcommitted
    SimOutput("MemoryWarning(): Overflow at " + to_string(machine_id) + " was detected at time " + to_string(time), 0);
}

void MigrationDone(Time_t time, VMId_t vm_id) {
    // The function is called on to alert you that migration is complete
    SimOutput("MigrationDone(): Migration of VM " + to_string(vm_id) + " was completed at time " + to_string(time), 4);
    Scheduler.MigrationComplete(time, vm_id);
    migrating = false;
}

void SchedulerCheck(Time_t time) {
    // This function is called periodically by the simulator, no specific event
    SimOutput("SchedulerCheck(): SchedulerCheck() called at " + to_string(time), 4);
    Scheduler.PeriodicCheck(time);
}

void SimulationComplete(Time_t time) {
    // This function is called before the simulation terminates Add whatever you feel like.
    cout << "SLA violation report" << endl;
    cout << "SLA0: " << GetSLAReport(SLA0) << "%" << endl;
    cout << "SLA1: " << GetSLAReport(SLA1) << "%" << endl;
    cout << "SLA2: " << GetSLAReport(SLA2) << "%" << endl;     // SLA3 do not have SLA violation issues
    cout << "Total Energy " << Machine_GetClusterEnergy() << "KW-Hour" << endl;
    cout << "Simulation run finished in " << double(time)/1000000 << " seconds" << endl;
    SimOutput("SimulationComplete(): Simulation finished at time " + to_string(time), 4);
    
    Scheduler.Shutdown(time);
}

void SLAWarning(Time_t time, TaskId_t task_id) {
    
}

void StateChangeComplete(Time_t time, MachineId_t machine_id) {
    MachineInfo_t machine_info = Machine_GetInfo(machine_id);
    RemoveMachineFromList(machine_id, machines_transitioning);
    
    if (machine_info.s_state == S0) {
        cout << "Machine " << machine_id << " now in S0" << endl;
        cout << "machine " << machine_id << " executing " << pending_tasks_for_machine[machine_id].size() << " pending tasks" << endl;
        ExecuteTasks(machine_id, scheduler->vms);
    } else if (machine_info.s_state == S2) {
        cout << "Machine " << machine_id << " now in S2" << endl;
        if (pending_tasks_for_machine[machine_id].size() != 0) {
            cout << "transitioning machine " << machine_id << " to S0" << endl;
            Machine_SetState(machine_id, S0);
        }
    }

    cout << "Machines running: " << machines_running.size() << endl;
    cout << "Machines standby: " << machines_standby.size() << endl;
    cout << "Machines sleeping: " << machines_sleeping.size() << endl;
}
