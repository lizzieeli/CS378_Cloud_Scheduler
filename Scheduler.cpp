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

vector<MachineId_t> ArmMachines;
vector<MachineId_t> PowerMachines;
vector<MachineId_t> RiscvMachines;
vector<MachineId_t> X86Machines;


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
    return curr_pending_time + additional_exec_time;
}

/* The purpose of these next four functions [AllocateNew__VM(TaskId_t, TaskInfo_t)] is to
   be able to dynamically reallocate resources being taken up by any idle VMs to a newly
   created required VM for a task when all other VMs of this required type is overcommitted.
*/
void Scheduler::AllocateNewLinuxVM(TaskId_t task_id, TaskInfo_t t_info) {
    // reached here, need to convert a compatible unused machine to a VM of this type instead

    for (unsigned i = 0; i < WinVms.size(); i++) {
        VMExecTimePair vm_pair = WinVms[i];
        VMInfo_t vm_info = VM_GetInfo(vm_pair.vm_id);
        MachineId_t m_id = vm_info.machine_id;
        if (vm_info.active_tasks.size() == 0 && Machine_GetInfo(m_id).cpu == t_info.required_cpu) {
            WinVms.erase(remove_if(WinVms.begin(), WinVms.end(),
                      [vm_pair](const VMExecTimePair& s) {
                        return s.vm_id == vm_pair.vm_id;
                      }), WinVms.end());
            vms.erase(remove(vms.begin(), vms.end(), vm_pair.vm_id), vms.end());
            VM_Shutdown(vm_info.vm_id);
            VMId_t new_vm = VM_Create(LINUX, t_info.required_cpu);
            VM_Attach(new_vm, m_id);
            LinuxVms.push_back({new_vm, FindRemainingExecTime(new_vm)});
            vms.push_back(new_vm);
            VM_AddTask(new_vm, task_id, HIGH_PRIORITY);
            return;
        }
    }

    for (unsigned i = 0; i < LinuxRTVms.size(); i++) {
        VMExecTimePair vm_pair = LinuxRTVms[i];
        VMInfo_t vm_info = VM_GetInfo(vm_pair.vm_id);
        MachineId_t m_id = vm_info.machine_id;
        if (vm_info.active_tasks.size() == 0 && Machine_GetInfo(m_id).cpu == t_info.required_cpu) {
            LinuxRTVms.erase(remove_if(LinuxRTVms.begin(), LinuxRTVms.end(),
                      [vm_pair](const VMExecTimePair& s) {
                        return s.vm_id == vm_pair.vm_id;
                      }), LinuxRTVms.end());
            vms.erase(remove(vms.begin(), vms.end(), vm_pair.vm_id), vms.end());
            VM_Shutdown(vm_info.vm_id);
            VMId_t new_vm = VM_Create(LINUX, t_info.required_cpu);
            VM_Attach(new_vm, m_id);
            LinuxVms.push_back({new_vm, FindRemainingExecTime(new_vm)});
            vms.push_back(new_vm);
            VM_AddTask(new_vm, task_id, HIGH_PRIORITY);
            return;
        }
    }

    for (unsigned i = 0; i < AixVms.size(); i++) {
        VMExecTimePair vm_pair = AixVms[i];
        VMInfo_t vm_info = VM_GetInfo(vm_pair.vm_id);
        MachineId_t m_id = vm_info.machine_id;
        if (vm_info.active_tasks.size() == 0 && Machine_GetInfo(m_id).cpu == t_info.required_cpu) {
            AixVms.erase(remove_if(AixVms.begin(), AixVms.end(),
                      [vm_pair](const VMExecTimePair& s) {
                        return s.vm_id == vm_pair.vm_id;
                      }), AixVms.end());
            vms.erase(remove(vms.begin(), vms.end(), vm_pair.vm_id), vms.end());
            VM_Shutdown(vm_info.vm_id);
            VMId_t new_vm = VM_Create(LINUX, t_info.required_cpu);
            VM_Attach(new_vm, m_id);
            LinuxVms.push_back({new_vm, FindRemainingExecTime(new_vm)});
            vms.push_back(new_vm);
            VM_AddTask(new_vm, task_id, HIGH_PRIORITY);
            return;
        }
    }
}

void Scheduler::AllocateNewWinVM(TaskId_t task_id, TaskInfo_t t_info) {

    for (unsigned i = 0; i < LinuxRTVms.size(); i++) {
        VMExecTimePair vm_pair = LinuxRTVms[i];
        VMInfo_t vm_info = VM_GetInfo(vm_pair.vm_id);
        MachineId_t m_id = vm_info.machine_id;
        if (vm_info.active_tasks.size() == 0 && Machine_GetInfo(m_id).cpu == t_info.required_cpu) {
            LinuxRTVms.erase(remove_if(LinuxRTVms.begin(), LinuxRTVms.end(),
                        [vm_pair](const VMExecTimePair& s) {
                        return s.vm_id == vm_pair.vm_id;
                        }), LinuxRTVms.end());
            vms.erase(remove(vms.begin(), vms.end(), vm_pair.vm_id), vms.end());
            VM_Shutdown(vm_info.vm_id);
            VMId_t new_vm = VM_Create(WIN, t_info.required_cpu);
            VM_Attach(new_vm, m_id);
            WinVms.push_back({new_vm, FindRemainingExecTime(new_vm)});
            vms.push_back(new_vm);
            VM_AddTask(new_vm, task_id, HIGH_PRIORITY);
            return;
        }
    }   

    for (unsigned i = 0; i < LinuxVms.size(); i++) {
        VMExecTimePair vm_pair = LinuxVms[i];
        VMInfo_t vm_info = VM_GetInfo(vm_pair.vm_id);
        MachineId_t m_id = vm_info.machine_id;
        if (vm_info.active_tasks.size() == 0 && Machine_GetInfo(m_id).cpu == t_info.required_cpu) {
            LinuxVms.erase(remove_if(LinuxVms.begin(), LinuxVms.end(),
                        [vm_pair](const VMExecTimePair& s) {
                        return s.vm_id == vm_pair.vm_id;
                        }), LinuxVms.end());
            vms.erase(remove(vms.begin(), vms.end(), vm_pair.vm_id), vms.end());
            VM_Shutdown(vm_info.vm_id);
            VMId_t new_vm = VM_Create(WIN, t_info.required_cpu);
            VM_Attach(new_vm, m_id);
            WinVms.push_back({new_vm, FindRemainingExecTime(new_vm)});
            vms.push_back(new_vm);
            VM_AddTask(new_vm, task_id, HIGH_PRIORITY);
            return;
        }
    }   


}

void Scheduler::AllocateNewLinuxRTVM(TaskId_t task_id, TaskInfo_t t_info) {
    for (unsigned i = 0; i < WinVms.size(); i++) {
        VMExecTimePair vm_pair = WinVms[i];
        VMInfo_t vm_info = VM_GetInfo(vm_pair.vm_id);
        MachineId_t m_id = vm_info.machine_id;
        if (vm_info.active_tasks.size() == 0 && Machine_GetInfo(m_id).cpu == t_info.required_cpu) {
            WinVms.erase(remove_if(WinVms.begin(), WinVms.end(),
                      [vm_pair](const VMExecTimePair& s) {
                        return s.vm_id == vm_pair.vm_id;
                      }), WinVms.end());
            vms.erase(remove(vms.begin(), vms.end(), vm_pair.vm_id), vms.end());
            VM_Shutdown(vm_info.vm_id);
            VMId_t new_vm = VM_Create(LINUX_RT, t_info.required_cpu);
            VM_Attach(new_vm, m_id);
            LinuxRTVms.push_back({new_vm, FindRemainingExecTime(new_vm)});
            vms.push_back(new_vm);
            VM_AddTask(new_vm, task_id, HIGH_PRIORITY);
            return;
        }
    }

    for (unsigned i = 0; i < AixVms.size(); i++) {
        VMExecTimePair vm_pair = AixVms[i];
        VMInfo_t vm_info = VM_GetInfo(vm_pair.vm_id);
        MachineId_t m_id = vm_info.machine_id;
        if (vm_info.active_tasks.size() == 0 && Machine_GetInfo(m_id).cpu == t_info.required_cpu) {
            AixVms.erase(remove_if(AixVms.begin(), AixVms.end(),
                      [vm_pair](const VMExecTimePair& s) {
                        return s.vm_id == vm_pair.vm_id;
                      }), AixVms.end());
            vms.erase(remove(vms.begin(), vms.end(), vm_pair.vm_id), vms.end());
            VM_Shutdown(vm_info.vm_id);
            VMId_t new_vm = VM_Create(LINUX_RT, t_info.required_cpu);
            VM_Attach(new_vm, m_id);
            LinuxRTVms.push_back({new_vm, FindRemainingExecTime(new_vm)});
            vms.push_back(new_vm);
            VM_AddTask(new_vm, task_id, HIGH_PRIORITY);
            return;
        }
    }

    for (unsigned i = 0; i < LinuxVms.size(); i++) {
        VMExecTimePair vm_pair = LinuxVms[i];
        VMInfo_t vm_info = VM_GetInfo(vm_pair.vm_id);
        MachineId_t m_id = vm_info.machine_id;
        if (vm_info.active_tasks.size() == 0 && Machine_GetInfo(m_id).cpu == t_info.required_cpu) {
            LinuxVms.erase(remove_if(LinuxVms.begin(), LinuxVms.end(),
                        [vm_pair](const VMExecTimePair& s) {
                        return s.vm_id == vm_pair.vm_id;
                        }), LinuxVms.end());
            vms.erase(remove(vms.begin(), vms.end(), vm_pair.vm_id), vms.end());
            VM_Shutdown(vm_info.vm_id);
            VMId_t new_vm = VM_Create(LINUX_RT, t_info.required_cpu);
            VM_Attach(new_vm, m_id);
            LinuxRTVms.push_back({new_vm, FindRemainingExecTime(new_vm)});
            vms.push_back(new_vm);
            VM_AddTask(new_vm, task_id, HIGH_PRIORITY);
            return;
        }
    }  
}

void Scheduler::AllocateNewAIXVM(TaskId_t task_id, TaskInfo_t t_info) {
    for (unsigned i = 0; i < LinuxRTVms.size(); i++) {
        VMExecTimePair vm_pair = LinuxRTVms[i];
        VMInfo_t vm_info = VM_GetInfo(vm_pair.vm_id);
        MachineId_t m_id = vm_info.machine_id;
        if (vm_info.active_tasks.size() == 0 && Machine_GetInfo(m_id).cpu == t_info.required_cpu) {
            LinuxRTVms.erase(remove_if(LinuxRTVms.begin(), LinuxRTVms.end(),
                        [vm_pair](const VMExecTimePair& s) {
                        return s.vm_id == vm_pair.vm_id;
                        }), LinuxRTVms.end());
            vms.erase(remove(vms.begin(), vms.end(), vm_pair.vm_id), vms.end());
            VM_Shutdown(vm_info.vm_id);
            VMId_t new_vm = VM_Create(AIX, t_info.required_cpu);
            VM_Attach(new_vm, m_id);
            AixVms.push_back({new_vm, FindRemainingExecTime(new_vm)});
            vms.push_back(new_vm);
            VM_AddTask(new_vm, task_id, HIGH_PRIORITY);
            return;
        }
    }   

    for (unsigned i = 0; i < LinuxVms.size(); i++) {
        VMExecTimePair vm_pair = LinuxVms[i];
        VMInfo_t vm_info = VM_GetInfo(vm_pair.vm_id);
        MachineId_t m_id = vm_info.machine_id;
        if (vm_info.active_tasks.size() == 0 && Machine_GetInfo(m_id).cpu == t_info.required_cpu) {
            LinuxVms.erase(remove_if(LinuxVms.begin(), LinuxVms.end(),
                        [vm_pair](const VMExecTimePair& s) {
                        return s.vm_id == vm_pair.vm_id;
                        }), LinuxVms.end());
            vms.erase(remove(vms.begin(), vms.end(), vm_pair.vm_id), vms.end());
            VM_Shutdown(vm_info.vm_id);
            VMId_t new_vm = VM_Create(AIX, t_info.required_cpu);
            VM_Attach(new_vm, m_id);
            AixVms.push_back({new_vm, FindRemainingExecTime(new_vm)});
            vms.push_back(new_vm);
            VM_AddTask(new_vm, task_id, HIGH_PRIORITY);
            return;
        }
    }   

}

void Scheduler::Init() {
    // Find the parameters of the clusters
    SimOutput("Scheduler::Init(): Total number of machines is " + to_string(Machine_GetTotal()), 3);
    SimOutput("Scheduler::Init(): Initializing scheduler", 1);

    // first, get the machine cluster information, specifically the different CPU types

    unsigned numARM = 0;
    unsigned numRISCV = 0;
    unsigned numPOWER = 0;
    unsigned numX86 = 0;

    unsigned total_machines = Machine_GetTotal();

    for (unsigned i = 0; i < total_machines; i++) {
        switch(Machine_GetCPUType(MachineId_t(i))) {
            case ARM:
                numARM++;
                machines.push_back(MachineId_t(i));
                ArmMachines.push_back(MachineId_t(i));
                break;
            case POWER:
                numPOWER++;
                machines.push_back(MachineId_t(i));
                PowerMachines.push_back(MachineId_t(i));
                break;
            case RISCV:
                numRISCV++;
                machines.push_back(MachineId_t(i));
                RiscvMachines.push_back(MachineId_t(i));
                break;
            case X86:
                numX86++;
                machines.push_back(MachineId_t(i));
                X86Machines.push_back(MachineId_t(i));
                break;
            default:
                break;
        }
    }

    // statically initialize VMs per machine

    for (unsigned i = 0; i < numARM; i++) {
        VMId_t vm_created;
        if (i >= 0 && i < numARM/2) {
            vm_created = VM_Create(WIN, ARM);
            VM_Attach(vm_created, ArmMachines[i]);
            WinVms.push_back({vm_created, FindRemainingExecTime(vm_created)});
        }
        else if (i == numARM/2 || i == (numARM/2 + 1)) {
            vm_created = VM_Create(LINUX_RT, ARM);
            VM_Attach(vm_created, ArmMachines[i]);
            LinuxRTVms.push_back({vm_created, FindRemainingExecTime(vm_created)});
        }
        else {
            vm_created = VM_Create(LINUX, ARM);
            VM_Attach(vm_created, ArmMachines[i]);
            LinuxVms.push_back({vm_created, FindRemainingExecTime(vm_created)});
         }
         vms.push_back(vm_created);
    }

    for (unsigned i = 0; i < numRISCV; i++) {
        VMId_t vm_created;
        if (i < numRISCV/2) {
            // initialize LINUX machine
            vm_created = VM_Create(LINUX, RISCV);
            VM_Attach(vm_created, RiscvMachines[i]);
            LinuxVms.push_back({vm_created, FindRemainingExecTime(vm_created)});
        }
        else {
            // initialize other machines 
            vm_created = VM_Create(LINUX_RT, RISCV);
            VM_Attach(vm_created, RiscvMachines[i]);
            LinuxRTVms.push_back({vm_created, FindRemainingExecTime(vm_created)});
        }
        vms.push_back(vm_created);
    }

    for (unsigned i = 0; i < numPOWER; i++) {
        VMId_t vm_created;
        if (i < numPOWER/2) {
            // initialize AIX machines
            vm_created = VM_Create(AIX, POWER);
            VM_Attach(vm_created, PowerMachines[i]);
            AixVms.push_back({vm_created, FindRemainingExecTime(vm_created)});
        }
        else if (i == numPOWER/2) {
            vm_created = VM_Create(LINUX_RT, POWER);
            VM_Attach(vm_created, PowerMachines[i]);
            LinuxRTVms.push_back({vm_created, FindRemainingExecTime(vm_created)});
        }
        else {
            vm_created = VM_Create(LINUX, POWER);
            VM_Attach(vm_created, PowerMachines[i]);
            LinuxVms.push_back({vm_created, FindRemainingExecTime(vm_created)});
        }
        vms.push_back(vm_created);
    }

    for (unsigned i = 0; i < numX86; i++) {
        VMId_t vm_created;
        if (i >= 0 && i < numX86/2) {
            // initialize LINUX machine
            vm_created = VM_Create(LINUX, X86);
            VM_Attach(vm_created, X86Machines[i]);
            LinuxVms.push_back({vm_created, FindRemainingExecTime(vm_created)});
        }
        else if (i == numX86/2 || i == (numX86/2 + 1)) {
            vm_created = VM_Create(LINUX_RT, X86);
            VM_Attach(vm_created, X86Machines[i]);
            LinuxRTVms.push_back({vm_created, FindRemainingExecTime(vm_created)});
        }
        else {
            vm_created = VM_Create(WIN, X86);
            VM_Attach(vm_created, X86Machines[i]);
            WinVms.push_back({vm_created, FindRemainingExecTime(vm_created)});
        }
        vms.push_back(vm_created);
    }
}

void Scheduler::MigrationComplete(Time_t time, VMId_t vm_id) {
    // Update your data structure. The VM now can receive new tasks
}

void Scheduler::NewTask(Time_t now, TaskId_t task_id) {
    // Turn on a machine, create a new VM, attach it to the VM, then add the task
    // Turn on a machine, migrate an existing VM from a loaded machine....
    // cout << "Received new task " << task_id << endl;
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
    for (VMExecTimePair vm_pair: vm_sorted_exec_time) {
        // get the adjusted vm exec time, based on this vm's mips and num cpus
        Time_t adjusted_time = FindAdjustedExecTime(task_id, vm_pair.vm_id, vm_pair.pending_execution_time);
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
        if (m_info.cpu == t_info.required_cpu && (m_info.memory_used + t_info.required_memory < m_info.memory_size)) {
            VM_AddTask(possible_vm, task_id, HIGH_PRIORITY);
            return;
        }
    }

    // reaching here means all other VMs of this task's required VM type are overcommitted.
    // need to convert a compatible unused machine to a VM of this type instead.
    switch(t_info.required_vm) {
        case LINUX:
            AllocateNewLinuxVM(task_id, t_info);
            break;
        case LINUX_RT:
            AllocateNewLinuxRTVM(task_id, t_info);
            break;
        case WIN:
            AllocateNewWinVM(task_id, t_info);
            break;
        case AIX:
            AllocateNewAIXVM(task_id, t_info);
            break;
        default:
            break;
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

    // relying on the most recent call of periodiccheck to update these to be accurate enough
    // at worst, what is currently there will be an overestimation
    vector<VMExecTimePair> OverloadedVMs;
    vector<VMExecTimePair> UnderloadedVMs;
    for (unsigned i = 0; i < LinuxVms.size()/2; i++) {
        UnderloadedVMs.push_back(LinuxVms[i]);
    }
    for (unsigned i = LinuxVms.size()/2; i < LinuxVms.size(); i++) {
        OverloadedVMs.push_back(LinuxVms[i]);
    }

    unsigned underloaded_index = 0;
    for (VMExecTimePair vm_pair: OverloadedVMs) {
        if (VM_GetInfo(vm_pair.vm_id).active_tasks.size() > 0) {
            TaskId_t task_to_migrate = VM_GetInfo(vm_pair.vm_id).active_tasks[0];
            for (unsigned i = underloaded_index; i < UnderloadedVMs.size(); i++) {
                if (Machine_GetInfo(VM_GetInfo(UnderloadedVMs[i].vm_id).machine_id).cpu == GetTaskInfo(task_id).required_cpu) {
                    // check for a threshold number, if the load between the two chosen is quite balanced already then just return
                    // else, continue on with load balancing
                    if (vm_pair.pending_execution_time - UnderloadedVMs[i].pending_execution_time <= 1000000) {
                        return;
                    }
                    cout << "Migrating task " << task_to_migrate << " from overloaded vm " << vm_pair.vm_id << " to underloaded vm " << UnderloadedVMs[i].vm_id << endl;
                    VM_RemoveTask(vm_pair.vm_id, task_to_migrate);
                    VM_AddTask(UnderloadedVMs[i].vm_id, task_to_migrate, HIGH_PRIORITY);
                    underloaded_index++;
                }
            } 
        }      
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
    
}

void StateChangeComplete(Time_t time, MachineId_t machine_id) {
    // Called in response to an earlier request to change the state of a machine
}