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

static vector<SimpleMachineInfo> SortMachinesByUtilization(vector<MachineId_t> machines, TaskInfo_t this_task_info, vector<VMId_t> vms) {
    // sort all active machines in a set of ascending order of their utilization
    vector<SimpleMachineInfo> sorted_active_machines;
    for (MachineId_t id: machines) {
        double this_utilization = GetMachineUtil(Machine_GetInfo(id), this_task_info, vms);
        sorted_active_machines.push_back({id, this_utilization});
    }

    sort(sorted_active_machines.begin(), sorted_active_machines.end(),
         [](const SimpleMachineInfo& a, SimpleMachineInfo& b){
                return a.utilization < b.utilization;
        });
    return sorted_active_machines;
}

static bool HandleSLAWarning(TaskId_t task_id, vector<MachineId_t> machines, vector<VMId_t> vms, VMId_t allocated_vm) {
    // sort all machines in ascending order of utilization
    TaskInfo_t this_task_info = GetTaskInfo(task_id);
    vector<SimpleMachineInfo> sorted_active_machines = SortMachinesByUtilization(machines, this_task_info, vms);
    bool allocation_success = false;
    for (unsigned i = 0; i < sorted_active_machines.size(); i++) {
        MachineId_t temp_m_id = sorted_active_machines[i].id;
        if (VM_GetInfo(allocated_vm).machine_id != temp_m_id) {
            MachineInfo_t m_info = Machine_GetInfo(temp_m_id);
            double m_util = GetMachineUtil(m_info, this_task_info, vms);
            double t_load = GetTaskLoadFactor(m_info, this_task_info, this_task_info.required_memory); 
            if (m_util + t_load < 1) {
                migrating = true;
                VM_Migrate(allocated_vm, temp_m_id);
                allocation_success = true;
            }
        }
    }
    // need to wake up new machine and put a new VM with this task on it if possible
    if (allocation_success == false) {
        MachineId_t new_machine_id = MachineId_t(active_machines);
        if (Machine_GetInfo(new_machine_id).cpu == this_task_info.required_cpu) {
            VMId_t new_vm_id = VM_Create(this_task_info.required_vm, this_task_info.required_cpu);
            active_machines++;
            cout << "new machine being turned on's id is " << new_machine_id << endl;
            cout << "new machine's CPU is " << Machine_GetInfo(new_machine_id).cpu << endl;
            cout << "required CPU for task is " << this_task_info.required_cpu << endl;
            cout << "required VM for task is " << this_task_info.required_vm << endl;
            VM_Attach(new_vm_id, new_machine_id);
            VM_AddTask(new_vm_id, task_id, HIGH_PRIORITY);
            task_to_vm[task_id] = new_vm_id;
            allocation_success = true;
        }
    }

    return allocation_success;
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

    // greedy does a "lazy" way of allocating, where we create one VM and attach to one machine until further notic
    active_machines = 1;
    vms.push_back(VM_Create(LINUX, X86));
    machines.push_back(MachineId_t(0));
    VM_Attach(vms[0], machines[0]);


    SimOutput("Scheduler::Init(): VM ids are " + to_string(vms[0]) + " and " + to_string(vms[1]), 3);
}

void Scheduler::MigrationComplete(Time_t time, VMId_t vm_id) {
    // Update your data structure. The VM now can receive new tasks
    vms.push_back(vm_id);
}

void Scheduler::NewTask(Time_t now, TaskId_t task_id) {
    if (SLA_violation == true) {
        // we have been called by SLA warning, need to respond to the SLA violation specified by task_id
        cout << "We have reached an SLA Violation with task " << task_id << endl;
        VMId_t VMThatHasViolation = task_to_vm[task_id];
        cout << "VM that has this violation of a task is " << VMThatHasViolation << endl;
        bool success = HandleSLAWarning(task_id, machines, vms, VMThatHasViolation);
        SLA_violation = false;
        cout << "success? " << success << endl;
        return;
    }

    /* this is essentially where the new requests will be handled */
    // Get the task parameters
    bool GPUneeded = IsTaskGPUCapable(task_id);
    CPUType_t req_CPU = RequiredCPUType(task_id);
    // SLAType_t req_SLA = RequiredSLA(task_id);
    VMType_t req_VM = RequiredVMType(task_id);
    unsigned req_mem = GetTaskMemory(task_id);
    TaskInfo_t this_task_info = GetTaskInfo(task_id);

    for (unsigned i = 0; i < active_machines; i++) {
        VMId_t this_VM_id = vms[i];
        VMInfo_t this_VM_info = VM_GetInfo(this_VM_id);
        if (this_VM_info.vm_type == req_VM) {
            MachineId_t this_machine_id= this_VM_info.machine_id;
            MachineInfo_t this_machine_info = Machine_GetInfo(this_machine_id);
            CPUType_t this_machine_CPU = Machine_GetCPUType(this_machine_id);

            if ((this_machine_info.s_state == S0) && (this_machine_CPU == req_CPU) && (GPUneeded? (this_machine_info.gpus? 1 : 0) : 1)) {
                // we know that we meet base level HW requirements
                // now look at utilization and load factor

                double machine_util = GetMachineUtil(this_machine_info, this_task_info, vms);
                double task_load_factor = GetTaskLoadFactor(this_machine_info, this_task_info, req_mem);

                if (machine_util + task_load_factor < 1) {
                    // place workload on this VM
                    cout << "Attaching task " << task_id << " to VM " << this_VM_id << " on machine " << this_VM_info.machine_id << endl;
                    VM_AddTask(this_VM_id, task_id, HIGH_PRIORITY);
                    task_to_vm[task_id] = this_VM_id;
                    return;
                }
            }
        }
    }
}

void Scheduler::PeriodicCheck(Time_t now) {
    // This method should be called from SchedulerCheck()
    // SchedulerCheck is called periodically by the simulator to allow you to monitor, make decisions, adjustments, etc.
    // Unlike the other invocations of the scheduler, this one doesn't report any specific event
    // Recommendation: Take advantage of this function to do some monitoring and adjustments as necessary
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

    // sort all active machines in a set of ascending order of their utilization
    vector<SimpleMachineInfo> sorted_active_machines = SortMachinesByUtilization(machines, this_task_info, vms);

    for (unsigned j = 0; j < sorted_active_machines.size(); j++) {
        MachineInfo_t this_m_info = Machine_GetInfo(sorted_active_machines[j].id);
        for (unsigned i = 0; i < this_m_info.active_vms; i++) {
            for (unsigned k = j; k < sorted_active_machines.size(); k++) {
                MachineInfo_t higher_util_m_info = Machine_GetInfo(sorted_active_machines[k].id);
                if (this_m_info.cpu == higher_util_m_info.cpu && this_m_info.gpus == higher_util_m_info.gpus) {
                    // double higher_util_m_util = GetMachineUtil(higher_util_m_info, this_task_info, vms);
                    // double this_task_load_factor = GetTaskLoadFactor(higher_util_m_info, this_task_info, this_task_info.required_memory);
                    // if (higher_util_m_util + this_task_load_factor < 1) {
                    //     migrating = true;
                    //     VM_Migrate(VMThatHasTask, higher_util_m_info.machine_id);
                    // }
                }
            }
        }
        if (this_m_info.active_vms == 0) {
            // no more active VM's, just shut off since no active tasks will be attached to the VM any longer
            Machine_SetState(this_m_info.machine_id, S5);
        }
    }

    // int iteration = 0;
    // // actually try to migrate tasks over if possible
    // for (SimpleMachineInfo mach: sorted_active_machines) {
    //     MachineInfo_t this_mach_info = Machine_GetInfo(mach.id);
    //     if (mach.utilization > 0 && this_mach_info.active_vms != 0) {
    //         for (VMId_t vm_id: vms) {
    //             VMInfo_t this_vm_info = VM_GetInfo(vm_id);
    //             if (this_vm_info.machine_id == mach.id) {
    //                 // for each vm attached to this machine
    //                 for (TaskId_t t_id: this_vm_info.active_tasks) {
    //                     TaskInfo_t temp_task_info = GetTaskInfo(t_id);
    //                     // for each workload on this attached vm
    //                     double load_factor = GetTaskLoadFactor(this_mach_info, temp_task_info, temp_task_info.required_memory);
    //                     for (unsigned i = iteration; i < sorted_active_machines.size(); i++) {
    //                         // for each machine with greater util than this current one
    //                         MachineInfo_t temp_mach_info = Machine_GetInfo(sorted_active_machines[i].id);
    //                         if (temp_task_info.required_cpu == temp_mach_info.cpu && (temp_task_info.gpu_capable? (temp_mach_info.gpus? 1 : 0) : 1)) {
    //                             // only try to check util and migrate if the machine even matches the specs of this task
    //                             double temp_util = GetMachineUtil(temp_mach_info, temp_task_info, vms);
    //                             if (load_factor + temp_util < 1) {
    //                                 // migrate this task to another VM on another machine
    //                                 // make sure that this VM and machine also matches the specs of this task first though
    //                                 for (VMId_t temp_vm: vms) {
    //                                     VMInfo_t temp_vm_info = VM_GetInfo(temp_vm);
    //                                     if (temp_vm_info.machine_id == sorted_active_machines[i].id && temp_task_info.required_vm == temp_vm_info.vm_type) {
    //                                         // migrating = true;
    //                                         // VM_RemoveTask(vm_id, t_id);
    //                                         // VM_AddTask(temp_vm_info.vm_id, t_id, HIGH_PRIORITY);
    //                                         // migrating = false;
    //                                         break;
    //                                     }
    //                                 }
    //                             }
    //                         }
    //                     }
    //                 }
    //                 // if (this_vm_info.active_tasks.size() == 0) {
    //                 //     VM_Shutdown(vm_id);
    //                 //     break;
    //                 // }
    //             }
    //         }
    //     }

    //     // // at the end, if there are no more vm's on this machine then turn it off
    //     // if ((Machine_GetInfo(mach.id)).active_vms == 0){
    //     //     // no more vms on this machine so turn it off and continue
    //     //     Machine_SetState(mach.id, S5);
    //     // }
    //     iteration++;
    // }
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
    // static unsigned counts = 0;
    // counts++;
    // if(counts == 10) {
    //     migrating = true;
    //     VM_Migrate(1, 9);
    // }
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
    SLA_violation = true;
    Scheduler.NewTask(time, task_id);
}

void StateChangeComplete(Time_t time, MachineId_t machine_id) {
    // Called in response to an earlier request to change the state of a machine
}

