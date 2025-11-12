//
//  Scheduler.cpp
//  CloudSim
//
//  Created by ELMOOTAZBELLAH ELNOZAHY on 10/20/24.
//

#include "Scheduler.hpp"
#include <algorithm>
#include <unordered_map>

static bool migrating = false;
static bool SLA_violation = false;
static unsigned active_machines = 1;
unordered_map<TaskId_t, VMId_t> task_to_vm;
vector<TaskId_t> todo_list;

struct SimpleMachineInfo {
    MachineId_t id;
    double utilization;
};

/* Helper functions */
static double GetTotalResources(MachineInfo_t m_info, TaskInfo_t t_info) {
    double mips_rating = m_info.performance[m_info.s_state] * 1000000;
    double time_remaining = (t_info.target_completion - t_info.arrival) / 1000000;
    double instr_possible_in_req_time = mips_rating * time_remaining;
    double total_resources = (m_info.memory_size + instr_possible_in_req_time);
    return total_resources;
}

static double GetTotalRemInstr(MachineInfo_t m_info, vector<VMId_t> vms) {
    unsigned remaining_instr = 0;
    for (unsigned i = 0; i < vms.size(); i++) {
        VMInfo_t this_vm_info = VM_GetInfo(vms[i]);
        if ((this_vm_info.machine_id == m_info.machine_id) && (m_info.s_state != S5)) {
            for (unsigned j = 0; j < this_vm_info.active_tasks.size(); j++) {
                TaskInfo_t this_active_task_info = GetTaskInfo(this_vm_info.active_tasks[j]);
                remaining_instr += this_active_task_info.remaining_instructions;
            }
        }
    }
    return remaining_instr;
}

static double GetMachineUtil(MachineInfo_t m_info, TaskInfo_t t_info, vector<VMId_t> vms) {
    double remaining_instr = GetTotalRemInstr(m_info, vms);
    double total_resources = GetTotalResources(m_info, t_info);

    double machine_util = (m_info.memory_used + remaining_instr) / total_resources; 
    return machine_util;
}

static double GetTaskLoadFactor(MachineInfo_t m_info, TaskInfo_t t_info, unsigned req_mem) {
    double total_resources = GetTotalResources(m_info, t_info);
    double task_load_factor = (req_mem + t_info.total_instructions) / total_resources;
    return task_load_factor;
}

 static double GetCPUUtil(MachineInfo_t m_info, vector<VMId_t>& vms) {
    double total_instr_remaining = 0;

    for (VMId_t vm_id : vms) {
        VMInfo_t vm_info = VM_GetInfo(vm_id);
        if (vm_info.machine_id != m_info.machine_id)
            continue;

        for (TaskId_t t_id : vm_info.active_tasks) {
            TaskInfo_t t_info = GetTaskInfo(t_id);
            total_instr_remaining += t_info.remaining_instructions;
        }
    }

    // Convert instructions to a fraction of machine capacity per second
    double machine_capacity = m_info.num_cpus * m_info.performance[S0] * 1e6; // instructions per second
    return total_instr_remaining / machine_capacity;
}

static double GetMemUtil(MachineInfo_t m_info, TaskInfo_t t_info) {
    // current memory used plus memory requested by new task
    double mem_util = double(m_info.memory_used + t_info.required_memory) / m_info.memory_size;
    return mem_util;
}

static vector<SimpleMachineInfo> SortMachinesByCPU(
    vector<MachineId_t>& machines,
    vector<VMId_t>& vms
) {
    vector<SimpleMachineInfo> sorted;
    for (MachineId_t id : machines) {
        MachineInfo_t m_info = Machine_GetInfo(id);
        double cpu_util = GetCPUUtil(m_info, vms);
        sorted.push_back({id, cpu_util});
    }

    sort(sorted.begin(), sorted.end(),
         [](const SimpleMachineInfo& a, const SimpleMachineInfo& b) {
             return a.utilization < b.utilization;
         });

    return sorted;
}

/* Internal/Private Scheduler Functions to implement */
void Scheduler::Init() {
    /* This is essentially where the initialization of all the diff data structures and stuff will happen */
    
    // Find the parameters of the clusters
    // Get the total number of machines
    // For each machine:
    //      Get the type of the machine
    //      Get the memory of the machine
    //      Get the number of CPUs
    //      Get if there is a GPU or not
    // 
    SimOutput("Scheduler::Init(): Total number of machines is " + to_string(Machine_GetTotal()), 3);
    SimOutput("Scheduler::Init(): Initializing scheduler", 1);

    active_machines = 1;
    vms.push_back(VM_Create(LINUX, X86));
    machines.push_back(MachineId_t(0));
    VM_Attach(vms[0], machines[0]);


    SimOutput("Scheduler::Init(): VM ids are " + to_string(vms[0]) + " and " + to_string(vms[1]), 3);
}

void Scheduler::MigrationComplete(Time_t time, VMId_t vm_id) {
}

static bool FindMachineForTask(vector<MachineId_t> machines, vector<VMId_t> vms, TaskId_t task_id) {
    vector<SimpleMachineInfo> sorted_machines = SortMachinesByCPU(machines, vms);

    TaskInfo_t this_task_info = GetTaskInfo(task_id);
    CPUType_t req_CPU = RequiredCPUType(task_id);
    VMType_t req_VM = RequiredVMType(task_id);
    unsigned req_mem = GetTaskMemory(task_id);


    // Step 2: Greedy assignment — machine first, then VM
    for (auto& entry : sorted_machines) {
        MachineId_t machine_id = entry.id;
        MachineInfo_t mach_info = Machine_GetInfo(machine_id);
    
        if (mach_info.cpu != req_CPU)
            continue;
        for (VMId_t vm_id : vms) {
            VMInfo_t vm_info = VM_GetInfo(vm_id);

            if (vm_info.machine_id != machine_id)
                continue;

            if (vm_info.vm_type != req_VM)
                continue;

            double cpu_util = GetCPUUtil(mach_info, vms);
            double mem_util = GetMemUtil(mach_info, this_task_info);

            cout << "Machine : " << machine_id << " in list with cpu " << cpu_util << endl;

            if (cpu_util < 1.0 && mem_util < 1.0) {
                cout << "Assign task " << task_id
                    << " to VM " << vm_id
                    << " on machine " << machine_id
                    << " (CPU=" << cpu_util << ", MEM=" << mem_util << ")\n";

                VM_AddTask(vm_id, task_id, HIGH_PRIORITY);
                task_to_vm[task_id] = vm_id;
                return true;
            }
        }
    }

    return false;
}


void Scheduler::PeriodicCheck(Time_t now) {
    // Checks if there's a task on the todo list that could be done
    for (auto it = todo_list.begin(); it != todo_list.end(); ) {
        TaskId_t task = *it;

        if (FindMachineForTask(machines, vms, task)) {
            it = todo_list.erase(it);
        } else {
            ++it;
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

    TaskInfo_t this_task_info = GetTaskInfo(task_id);
    VMId_t VMThatHasTask = task_to_vm[task_id];
    task_to_vm.erase(task_id);
}

// Static helper function to add a new machine & VM if no existing VM can take the task
static void AddActiveMachine(TaskId_t task_id, TaskInfo_t task_info,
                             std::vector<MachineId_t>& machines,
                             std::vector<VMId_t>& vms,
                             std::unordered_map<TaskId_t, VMId_t>& task_to_vm,
                             unsigned& active_machines) {
    unsigned total = Machine_GetTotal();

    for (unsigned i = 0; i < total; i++) {
        // Skip machines already active
        if (std::find(machines.begin(), machines.end(), i) != machines.end())
            continue;

        MachineInfo_t m_info = Machine_GetInfo(i);

        // Check CPU compatibility
        if (m_info.cpu != task_info.required_cpu)
            continue;

        machines.push_back(i);
        active_machines++;

        VMId_t new_vm_id = VM_Create(task_info.required_vm, task_info.required_cpu);
        vms.push_back(new_vm_id);
        VM_Attach(new_vm_id, i);
        VM_AddTask(new_vm_id, task_id, HIGH_PRIORITY);

        task_to_vm[task_id] = new_vm_id;

        cout << "Activated machine " << i << " and placed task " << task_id
                  << " on VM " << new_vm_id << endl;
        return;
    }

    // no machines are currently suitable to run this task; push to pending list
    todo_list.push_back(task_id);
}

// Greedy machine-first NewTask implementation
void Scheduler::NewTask(Time_t now, TaskId_t task_id) {
    TaskInfo_t this_task_info = GetTaskInfo(task_id);
    CPUType_t req_CPU = RequiredCPUType(task_id);
    VMType_t req_VM = RequiredVMType(task_id);
    unsigned req_mem = GetTaskMemory(task_id);

    // Tries to find machine that can accept the task
    if (!FindMachineForTask(machines, vms, task_id)) {
        // No machines found; try to create a new machine and attach a new vm
        AddActiveMachine(task_id, this_task_info, machines, vms, task_to_vm, active_machines);
    }
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
    // call a function that will handle SLA warnings internally by the scheduler
    // SLA_violation = true;
    // Scheduler.NewTask(time, task_id);
}

void StateChangeComplete(Time_t time, MachineId_t machine_id) {
    // Called in response to an earlier request to change the state of a machine
}


