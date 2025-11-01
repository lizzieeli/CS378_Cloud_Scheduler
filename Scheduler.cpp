//
//  Scheduler.cpp
//  CloudSim
//
//  Created by ELMOOTAZBELLAH ELNOZAHY on 10/20/24.
//

#include "Scheduler.hpp"
#include <vector>

static bool migrating = false;
static unsigned active_machines = 16;

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

    cout << "initializing scheduler with " << Machine_GetTotal() << " machines." << endl;

    for(unsigned i = 0; i < active_machines; i++) {
        vms.push_back(VM_Create(LINUX, X86));
    }
    for(unsigned i = 0; i < active_machines; i++) {
        machines.push_back(MachineId_t(i));
    }    
    for(unsigned i = 0; i < active_machines; i++) {
        VM_Attach(vms[i], machines[i]);
    }

    // bool dynamic = false;
    // if(dynamic)
    //     for(unsigned i = 0; i<4 ; i++)
    //         for(unsigned j = 0; j < 8; j++)
    //             // can't lie i don't see the point in having this line be in the double for loop
    //             // if we are ignoring the core_id anyway and the helper basically sets all cpus
    //             // on machine with id 0 to p3 state anyway with the single call??
    //             Machine_SetCorePerformance(MachineId_t(0), j, P3);


    // Turn off the ARM machines
    // all other machines at this point are inactive
    for(unsigned i = active_machines; i < Machine_GetTotal(); i++)
        Machine_SetState(MachineId_t(i), S5);

    SimOutput("Scheduler::Init(): VM ids are " + to_string(vms[0]) + " and " + to_string(vms[1]), 3);
}

void Scheduler::MigrationComplete(Time_t time, VMId_t vm_id) {
    // Update your data structure. The VM now can receive new tasks
}

void Scheduler::NewTask(Time_t now, TaskId_t task_id) {
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
        // cout << "Checking VM " << to_string(vms[i]) << endl;
        VMInfo_t this_VM_info = VM_GetInfo(this_VM_id);
        // cout << "this vm's type: " << to_string(this_VM_info.vm_type) << endl;
        // cout << "required vm type: " << to_string(req_VM) << endl;
        if (this_VM_info.vm_type == req_VM) {
            MachineId_t this_machine_id= this_VM_info.machine_id;
            MachineInfo_t this_machine_info = Machine_GetInfo(this_machine_id);
            CPUType_t this_machine_CPU = Machine_GetCPUType(this_machine_id);

            // cout << "this attached machines's CPU type: " << to_string(this_machine_CPU) << endl;
            // cout << "required CPU type: " << to_string(req_CPU) << endl;

            // cout << "does this machine have GPUS: " << this_machine_info.gpus << endl;
            // cout << "required GPUS?: " << GPUneeded << endl;

            if ((this_machine_info.s_state != S5) && (this_machine_CPU == req_CPU) && (GPUneeded? (this_machine_info.gpus? 1 : 0) : 1)) {
                // we know that we meet base level HW requirements
                // now look at utilization and load factor
                // cout << "We have reached a qualified VM and Machine, now we check for the load factor: " << endl;
                unsigned remaining_instr = 0;
                for (unsigned i = 0; i < this_VM_info.active_tasks.size(); i++) {
                    TaskInfo_t this_active_task_info = GetTaskInfo(this_VM_info.active_tasks[i]);
                    remaining_instr += this_active_task_info.remaining_instructions;
                }
                double mips_rating = this_machine_info.performance[this_machine_info.s_state] * 1000000;
                double time_remaining = (this_task_info.target_completion - this_task_info.arrival) / 1000000;
                double instr_possible_in_req_time = mips_rating * time_remaining;
                double total_resources = (this_machine_info.memory_size + instr_possible_in_req_time); // instr_possible_in_req_time;

                double machine_util = (this_machine_info.memory_used + remaining_instr) / total_resources; // remaining_instr / total_resources;
                double task_load_factor = (req_mem + this_task_info.total_instructions) / total_resources; // this_task_info.total_instructions / total_resources;

                /* ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~Debugging comments~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ */
                // cout << "completion time of task : " << this_task_info.target_completion << endl;
                // cout << "arrival time of task : " << this_task_info.arrival << endl;
                // cout << "time remaining of this task in seconds: " << time_remaining << endl;
                // cout << "remaining instr on machine: " << remaining_instr << endl;
                // cout << "this tasks's total instr: " << this_task_info.total_instructions << endl;
                // cout << "total isntr possible in requested time: " << instr_possible_in_req_time << endl;
                // cout << "current machine util: " << machine_util << endl;
                // cout << "current task load factor: " << task_load_factor << endl;
                /* ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ */

                if (machine_util + task_load_factor < 1) {
                    // place workload on this VM
                    // giving every task a high priority for this one because greedy doesn't really
                    // specify a priority type? Maybe we can change later to prioritize shortest jobs first?
                    cout << "Attaching task " << task_id << " to VM " << this_VM_id << endl;
                    VM_AddTask(this_VM_id, task_id, HIGH_PRIORITY);
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

    // sort all machines in a set of ascending order of their utilization

    // for all machines in the set where the utilization is > 0
        // for each workload in the given machine
            // for all other machines 
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
    static unsigned counts = 0;
    counts++;
    if(counts == 10) {
        migrating = true;
        VM_Migrate(1, 9);
    }
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
    // TODO: implement handling SLA warnings
    // sort all machines in ascending order of utilization
    // find a machine that can accommodate the load factor of i
        // if found, migrate the workload to that specific machine
}

void StateChangeComplete(Time_t time, MachineId_t machine_id) {
    // Called in response to an earlier request to change the state of a machine
}

