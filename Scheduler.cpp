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

struct MachineStatePair {
    MachineId_t id;
    MachineState_t s_state;
};

struct MachineMemoryPair {
    MachineId_t id;
    unsigned memory_available;
};

struct VMExecTimePair {
    VMId_t vm_id;
    Time_t pending_execution_time;
};

vector<MachineStatePair> ARMTotal;
vector<MachineStatePair> POWERTotal;
vector<MachineStatePair> RISCVTotal;
vector<MachineStatePair> X86Total;

vector<MachineMemoryPair> sorted_machines_by_mem;
vector<VMId_t> migrating_VMs;
vector<MachineId_t> state_changing_machines;

/* helper functions */
static Time_t FindRemainingExecTime(VMId_t this_vm){
    VMInfo_t vm_info = VM_GetInfo(this_vm);
    uint64_t total_remaining_instr = 0;
    for (TaskId_t active_task: vm_info.active_tasks) {
        total_remaining_instr += GetTaskInfo(active_task).remaining_instructions;
    }
    MachineInfo_t m_info = Machine_GetInfo(vm_info.machine_id);
    unsigned int instructions_per_sec = m_info.performance[m_info.p_state] * 1000000;
    // get the MIPS rating so we can do remaining_instr / MIPS to get seconds remaining for a given task
    Time_t remaining_exec_time = (total_remaining_instr / instructions_per_sec) * 1000000; // conversion from seconds to microseconds
    return remaining_exec_time; // in microseconds
}

void Scheduler::Init() {
    // Find the parameters of the clusters
    SimOutput("Scheduler::Init(): Total number of machines is " + to_string(Machine_GetTotal()), 3);
    SimOutput("Scheduler::Init(): Initializing scheduler", 1);

    unsigned int total_machines = Machine_GetTotal();
    for (unsigned i = 0; i < total_machines; i++) {
        MachineId_t m_id = MachineId_t(i);
        MachineInfo_t m_info = Machine_GetInfo(m_id);
        switch (m_info.cpu) {
            case ARM:
                ARMTotal.push_back({m_id, m_info.s_state});
                break;
            case POWER:
                POWERTotal.push_back({m_id, m_info.s_state});
                break;
            case RISCV:
                RISCVTotal.push_back({m_id, m_info.s_state});
                break;
            case X86:
                X86Total.push_back({m_id, m_info.s_state});
                break;
            default:
                break;
        }
        // all machines should have low memory usage
        unsigned int mem_available = m_info.memory_size - m_info.memory_used;
        sorted_machines_by_mem.push_back({m_id, mem_available});
    }

    // sort memory available by most amount to least amount
    sort(sorted_machines_by_mem.begin(), sorted_machines_by_mem.end(),
        [](const MachineMemoryPair& a, MachineMemoryPair& b){
            return a.memory_available > b.memory_available;
        });

    // initialize 1 VM to start for now
    VMId_t X86_vm = VM_Create(LINUX, X86);
    vms.push_back(X86_vm);
    machines.push_back(X86Total[0].id);
    VM_Attach(X86_vm, X86Total[0].id);

}

void Scheduler::MigrationComplete(Time_t time, VMId_t vm_id) {
    // Update your data structure. The VM now can receive new tasks
}

void Scheduler::NewTask(Time_t now, TaskId_t task_id) {
    // Turn on a machine, create a new VM, attach it to the VM, then add the task
    // Turn on a machine, migrate an existing VM from a loaded machine....

    vector<VMExecTimePair> vm_sorted_exec_time;

    // sort all active (not migrating) VMs that are on active (not state changing) machines by their pending execution times
    for (VMId_t vm_id: vms) {
        VMInfo_t vm_info = VM_GetInfo(vm_id);
        auto it1 = find(migrating_VMs.begin(), migrating_VMs.end(), vm_id);
        auto it2 = find(state_changing_machines.begin(), state_changing_machines.end(), vm_info.machine_id);
        if (it1 == migrating_VMs.end() && it2 == state_changing_machines.end()) {
            // this vm is currently not migrating and the machine it is on is not changing state either, so we 
            // should consider it for our list of available vm's from list of active vms, figure out the 
            // remaining execution time from all of the vm's active tasks
            Time_t pending_execution_time = FindRemainingExecTime(vm_id);
            vm_sorted_exec_time.push_back({vm_id, pending_execution_time});
        }
    }

    // now sort the list by ascending execution times
    sort(vm_sorted_exec_time.begin(), vm_sorted_exec_time.end(),
        [](const VMExecTimePair& a, VMExecTimePair& b){
            return a.pending_execution_time < b.pending_execution_time;
        });

    // now, based on this list, pick the next compatible vm with the lowest pending execution time
    VMId_t selected_v = vm_sorted_exec_time[0].vm_id;
    

    Priority_t priority = (task_id == 0 || task_id == 64)? HIGH_PRIORITY : MID_PRIORITY;
    if(migrating) {
        VM_AddTask(vms[0], task_id, priority);
    }
    else {
        VM_AddTask(vms[0], task_id, priority);
    }// Skeleton code, you need to change it according to your algorithm
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