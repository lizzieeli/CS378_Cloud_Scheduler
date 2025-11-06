//
//  Scheduler.cpp
//  CloudSim
//
//  Created by ELMOOTAZBELLAH ELNOZAHY on 10/20/24.
//

#include "Scheduler.hpp"
#include <map>
#include <unordered_map>
#include <algorithm>

static bool migrating = false;



struct VMExecTimePair {
    VMId_t vm_id;
    Time_t pending_execution_time;
};

// this will be the pool of VMs we can choose from
// starting with basic implementation of 1 VM per machine for now
// these will all be sorted in ascending pending execution time order
vector<VMExecTimePair> LinuxVms;
vector<VMExecTimePair> LinuxRTVms;
vector<VMExecTimePair> WinVms;
vector<VMExecTimePair> AixVms;


/* helper functions */

/*  The point of this function is to calculate the pending execution time of a given VM.
    It goes through to get all the remaining total instructions left (from all its active tasks)
    then gets the MIPS based on the current p state of the machine this vm is attached to.
    Additionally, we need to get the total number of cpus that the physical machine has.
    From there, we can calculate the remaining expected time of execution from this point.
*/
static Time_t FindRemainingExecTime(VMId_t this_vm){
    VMInfo_t vm_info = VM_GetInfo(this_vm);
    uint64_t total_remaining_instr = 0;
    for (TaskId_t active_task: vm_info.active_tasks) {
        total_remaining_instr += GetTaskInfo(active_task).remaining_instructions;
    }
    MachineInfo_t m_info = Machine_GetInfo(vm_info.machine_id);
    unsigned int instructions_per_sec = m_info.performance[m_info.p_state] * 1000000;
    // get the MIPS rating so we can do remaining_instr / MIPS to get seconds remaining for a given task
    Time_t remaining_exec_time = (total_remaining_instr / (instructions_per_sec * m_info.num_cpus)) * 1000000; // conversion from seconds to microseconds
    cout << "vm " << this_vm << " pending exec time is " << remaining_exec_time << endl;
    return remaining_exec_time; // in microseconds
}

/* The purpose of this function is to calculate the adjusted pending execution time
    by taking the current pending time of the given vm, then depending on the mips and
    number of cpus attached to this vm, these metrics can determine however long
    it will take for this task to finish given the current state of the vm.
    The function will return the adjusted amount from the given current pending time.
*/
static Time_t FindAdjustedExecTime(TaskId_t task_id, VMId_t vm_id, Time_t curr_pending_time) {
    VMInfo_t vm_info = VM_GetInfo(vm_id);
    MachineInfo_t m_info = Machine_GetInfo(vm_info.machine_id);
    TaskInfo_t t_info = GetTaskInfo(task_id);
    unsigned int instructions_per_sec = m_info.performance[m_info.p_state] * 1000000;
    Time_t additional_exec_time = (t_info.remaining_instructions / (instructions_per_sec * m_info.num_cpus)) * 1000000;
    cout << "task " << task_id << " additional exec time in microseconds is " << additional_exec_time << endl;
    return curr_pending_time + additional_exec_time;
}

void Scheduler::Init() {
    // Find the parameters of the clusters
    SimOutput("Scheduler::Init(): Total number of machines is " + to_string(Machine_GetTotal()), 3);
    SimOutput("Scheduler::Init(): Initializing scheduler", 1);

    // first, get the machine cluster information, specifically the different CPU types
    // unsigned numARM = 0;
    // unsigned numRISCV = 0;
    // unsigned numPOWER = 0;
    // unsigned numX86 = 0;

    // unsigned total_machines = Machine_GetTotal();

    for (int i = 0; i < 16; i++) {
        machines.push_back(MachineId_t(i));
        VMId_t vm_created = VM_Create(LINUX, X86);
        vms.push_back(vm_created);
        VM_Attach(vm_created, MachineId_t(i));
        LinuxVms.push_back({vm_created, FindRemainingExecTime(vm_created)});
    }

    // for (unsigned i = 0; i < total_machines; i++) {
    //     VMId_t vm_created;
    //     switch(Machine_GetCPUType(MachineId_t(i))) {
    //         case ARM:
    //             numARM++;
    //             machines.push_back(MachineId_t(i));
    //             break;
    //         case POWER:
    //             numPOWER++;
    //             machines.push_back(MachineId_t(i));
    //             break;
    //         case RISCV:
    //             numRISCV++;
    //             machines.push_back(MachineId_t(i));
    //             break;
    //         case X86:
    //             numX86++;
    //             machines.push_back(MachineId_t(i));
    //             vm_created = VM_Create(LINUX, X86);
    //             vms.push_back(vm_created);
    //             VM_Attach(vm_created, MachineId_t(i));
    //             LinuxVms.push_back({vm_created, FindRemainingExecTime(vm_created)});
    //             break;
    //         default:
    //             break;
    //     }
    // }

}

void Scheduler::MigrationComplete(Time_t time, VMId_t vm_id) {
    // Update your data structure. The VM now can receive new tasks
}

void Scheduler::NewTask(Time_t now, TaskId_t task_id) {
    // Turn on a machine, create a new VM, attach it to the VM, then add the task
    // Turn on a machine, migrate an existing VM from a loaded machine....
    TaskInfo_t t_info = GetTaskInfo(task_id);
    vector<VMExecTimePair> vm_sorted_exec_time;

    // only looking through the pool of vms that match this task's required vm type
    // may as well update all the pending execution times now too
    switch(t_info.required_vm) {
        case LINUX:
            for (VMExecTimePair vm_pair: LinuxVms) {
                Time_t pending_execution_time = FindRemainingExecTime(vm_pair.vm_id);
                vm_sorted_exec_time.push_back({vm_pair.vm_id, pending_execution_time});
            }
            break;
        case LINUX_RT:
            for (VMExecTimePair vm_pair: LinuxRTVms) {
                Time_t pending_execution_time = FindRemainingExecTime(vm_pair.vm_id);
                vm_sorted_exec_time.push_back({vm_pair.vm_id, pending_execution_time});
            }
            break;
        case WIN:
            for (VMExecTimePair vm_pair: WinVms) {
                Time_t pending_execution_time = FindRemainingExecTime(vm_pair.vm_id);
                vm_sorted_exec_time.push_back({vm_pair.vm_id, pending_execution_time});
            }
            break;
        case AIX:
            for (VMExecTimePair vm_pair: AixVms) {
                Time_t pending_execution_time = FindRemainingExecTime(vm_pair.vm_id);
                vm_sorted_exec_time.push_back({vm_pair.vm_id, pending_execution_time});
            }
            break;
        default:
            break;
    }

    // sort all active (not migrating) VMs that are on active machines by their 
    // pending execution times in ascending order
    sort(vm_sorted_exec_time.begin(), vm_sorted_exec_time.end(),
        [](const VMExecTimePair& a, VMExecTimePair& b){
            return a.pending_execution_time < b.pending_execution_time;
        });

    vector<VMExecTimePair> adjusted_vm_exec_times;
    // now, go through every vm on this list and add
    cout << "Now we are calculating and adjusting every available VM's adjusted execution time with this task" << endl;
    for (VMExecTimePair vm_pair: vm_sorted_exec_time) {
        // get the adjusted vm exec time, based on this vm's mips and num cpus
        Time_t adjusted_time = FindAdjustedExecTime(task_id, vm_pair.vm_id, vm_pair.pending_execution_time);
        cout << "The new adjusted execution time for vm " << vm_pair.vm_id << " is " << adjusted_time << endl;
        // put that in auxiliary structure as a candidate to consider
        adjusted_vm_exec_times.push_back({vm_pair.vm_id, adjusted_time});
    }
    
    // sort this list of adjusted times now with the added in weight of the VM capabilities with task's demands
    sort(adjusted_vm_exec_times.begin(), adjusted_vm_exec_times.end(),
        [](const VMExecTimePair& a, VMExecTimePair& b){
            return a.pending_execution_time < b.pending_execution_time;
        });


    // now just choose the first vm that matches cpu description
    for (unsigned i = 0; i < adjusted_vm_exec_times.size(); i++) {
        VMId_t possible_vm = adjusted_vm_exec_times[i].vm_id;
        MachineInfo_t m_info = Machine_GetInfo(VM_GetInfo(possible_vm).machine_id);
        if (m_info.cpu == t_info.required_cpu) {
            // found a good match
            // TODO: calculate a better priority, probably based on
            // the target_completion time in comparison to the now time
            // or something like that
            // update the vectors maybe or maybe that is not entirely necessary
            // cout << "found a valid VM on valid machine. adding task to VM " << possible_vm 
            //      << " with execution time of " << adjusted_vm_exec_times[i].pending_execution_time << endl;
            VM_AddTask(possible_vm, task_id, HIGH_PRIORITY);
            return;
            // if (t_info.required_vm == LINUX) {
                
            // }
            // else if (t_info.required_vm == LINUX_RT) {

            // }
            // else if (t_info.required_vm == WIN) {

            // }
            // else { // needs to be AIX atp

            // }
        }
    }
}

void Scheduler::PeriodicCheck(Time_t now) {
    // This method should be called from SchedulerCheck()
    // SchedulerCheck is called periodically by the simulator to allow you to monitor, make decisions, adjustments, etc.
    // Unlike the other invocations of the scheduler, this one doesn't report any specific event
    // Recommendation: Take advantage of this function to do some monitoring and adjustments as necessary

    // we will use this to periodically update our pending execution time of each of our VMs
    // vector<VMExecTimePair> temp_linux;
    // vector<VMExecTimePair> temp_linuxrt;
    // vector<VMExecTimePair> temp_win;
    // vector<VMExecTimePair> temp_aix;

    // // for linux vms
    // for (VMExecTimePair vm_pair: LinuxVms) {
    //     Time_t pending_execution_time = FindRemainingExecTime(vm_pair.vm_id);
    //     temp_linux.push_back({vm_pair.vm_id, pending_execution_time});
    // }
    // sort(temp_linux.begin(), temp_linux.end(),
    // [](const VMExecTimePair& a, VMExecTimePair& b){
    //     return a.pending_execution_time < b.pending_execution_time;
    // });

    // // for linux rt vms
    // for (VMExecTimePair vm_pair: LinuxRTVms) {
    //     Time_t pending_execution_time = FindRemainingExecTime(vm_pair.vm_id);
    //     temp_linuxrt.push_back({vm_pair.vm_id, pending_execution_time});
    // }
    // sort(temp_linuxrt.begin(), temp_linuxrt.end(),
    // [](const VMExecTimePair& a, VMExecTimePair& b){
    //     return a.pending_execution_time < b.pending_execution_time;
    // });

    // // for windows vms
    // for (VMExecTimePair vm_pair: WinVms) {
    //     Time_t pending_execution_time = FindRemainingExecTime(vm_pair.vm_id);
    //     temp_win.push_back({vm_pair.vm_id, pending_execution_time});
    // }
    // sort(temp_win.begin(), temp_win.end(),
    // [](const VMExecTimePair& a, VMExecTimePair& b){
    //     return a.pending_execution_time < b.pending_execution_time;
    // });

    // // for aix vms
    // for (VMExecTimePair vm_pair: AixVms) {
    //     Time_t pending_execution_time = FindRemainingExecTime(vm_pair.vm_id);
    //     temp_aix.push_back({vm_pair.vm_id, pending_execution_time});
    // }
    // sort(temp_aix.begin(), temp_aix.end(),
    // [](const VMExecTimePair& a, VMExecTimePair& b){
    //     return a.pending_execution_time < b.pending_execution_time;
    // });
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

    // TODO: add in optimizations that the paper talks about
    // with load balancing
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
    // Called in response to an earlier request to change the state of a machine
}