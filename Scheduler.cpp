//
//  Scheduler.cpp
//  CloudSim
//
//  Created by ELMOOTAZBELLAH ELNOZAHY on 10/20/24.
//

#include "Scheduler.hpp"
#include <algorithm>
#include <map>

/* Using this to define a template that defines upper limit of resources
   that this given task will set up.
*/
struct task_template {
    bool compute_task; // 1 for compute intensive, 0 for IO intensive
    unsigned memory_to_allocate;
    Time_t target_time;
    uint64_t instructions_to_execute;
    bool gpu_enabled;
};

/* This is gonna be for compute intensive machines */
struct VMResourcePair {
    VMId_t vm_id;
    Time_t pending_execution_time;
    unsigned avail_mem;
};

/* continually sort these by pending execution time 
   from smallest pending time to longest pending time */
vector<VMResourcePair> Compute_Linux;
vector<VMResourcePair> Compute_Linuxrt;
vector<VMResourcePair> Compute_Win;
vector<VMResourcePair> Compute_Aix;

vector<task_template> Compute_Linux_Templates;
vector<task_template> Compute_Linuxrt_Templates;
vector<task_template> Compute_Win_Templates;
vector<task_template> Compute_Aix_Templates;

/* continually sort these by available memory left 
   from most memory to least memory available */
vector<VMResourcePair> IO_Linux;
vector<VMResourcePair> IO_Linuxrt;
vector<VMResourcePair> IO_Win;
vector<VMResourcePair> IO_Aix;

vector<task_template> IO_Linux_Templates;
vector<task_template> IO_Linuxrt_Templates;
vector<task_template> IO_Win_Templates;
vector<task_template> IO_Aix_Templates;

unsigned maxNumCPUS;
unsigned maxMIPS;
unsigned maxMemory;

/*  The point of this function is to calculate the pending execution time of a given VM.
    It goes through to get all the remaining total instructions left (from all its active tasks)
    then gets the MIPS based on the current p state of the machine this vm is attached to.
    Additionally, we need to get the total number of cpus that the physical machine has.
    From there, we can calculate the remaining expected time of execution from this point.
*/
static Time_t FindRemainingExecTime(VMId_t this_vm) {
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

/*  The point of this function is to calculate the remaining
    available memory of a given VM attached to a machine,
    given the assumption that one machine has only one VM
    (can implement migrating later maybe if time)
*/
static unsigned FindRemainingAvailMem(VMId_t vm_id) {
    VMInfo_t vm_info = VM_GetInfo(vm_id);
    MachineInfo_t m_info = Machine_GetInfo(vm_info.machine_id);
    return m_info.memory_size - m_info.memory_used;
}

/*  The point of this function is to calculate the adjusted
    remaining available memory of a given VM after assigning this
    task, given the assumption that one machine has only one VM
    (can implement migrating later maybe if time)
*/
static unsigned FindAdjustedAvailMem(TaskId_t task_id, VMId_t vm_id, unsigned curr_available_mem) {
    TaskInfo_t t_info = GetTaskInfo(task_id);
    return curr_available_mem - t_info.required_memory;
}

/* This function is meant to extract a template of necessary resources and
   to identify whether the following task is a compute intensive or io intensive task.
   Based on this identification, we will have a different energy saving policy for the
   machine it is attached to.
*/
static task_template TemplateExtraction(TaskId_t t_id, Time_t now) {
    TaskInfo_t t_info = GetTaskInfo(t_id);
    double target_execution_time = (t_info.target_completion - now) / 1000000; // microseconds
    double millions_of_instr = t_info.total_instructions / 1000000; // millions of instr
    task_template new_template;
    if (t_info.required_memory > (0.6 * maxMemory)) {
        // this is a memory/io intensive task
        new_template.compute_task = 0;
    }
    else if ((millions_of_instr / target_execution_time)  > (0.6 * maxMIPS)) {
        // this is a compute intensive task
        new_template.compute_task = 1;
    }
    else {
        // this is a balanced task, compare the ratios and tiebreak to compute task
        double memory_requirement_ratio = t_info.required_memory / maxMemory;
        double mips_requirement_ratio = (millions_of_instr / target_execution_time) / maxMIPS;
        memory_requirement_ratio > mips_requirement_ratio ? 
            (new_template.compute_task = 0) : (new_template.compute_task = 1);
    }
    new_template.memory_to_allocate = t_info.required_memory;
    new_template.target_time = t_info.target_completion;
    new_template.instructions_to_execute = t_info.total_instructions;
    new_template.gpu_enabled = t_info.gpu_capable;
    return new_template;
}

/* TODO: add in all the helper functions that will dynamically
   reallocate idle VMs of a certain type to one of another type
   when there are overcommitted machines
*/

void Scheduler::AllocateNewLinuxVM(TaskId_t task_id, TaskInfo_t t_info, bool compute_task) {
    if (compute_task) {
        // pull from other compute VMs first that are idle
        for (unsigned i = 0; i < Compute_Win.size(); i++) {
            VMResourcePair vm_pair = Compute_Win[i];
            VMInfo_t vm_info = VM_GetInfo(vm_pair.vm_id);
            MachineId_t m_id = vm_info.machine_id;
            if (vm_info.active_tasks.size() == 0 && Machine_GetInfo(m_id).cpu == t_info.required_cpu) {
                Compute_Win.erase(remove_if(Compute_Win.begin(), Compute_Win.end(),
                        [vm_pair](const VMResourcePair& s) {
                            return s.vm_id == vm_pair.vm_id;
                        }), Compute_Win.end());
                vms.erase(remove(vms.begin(), vms.end(), vm_pair.vm_id), vms.end());
                VM_Shutdown(vm_info.vm_id);
                VMId_t new_vm = VM_Create(LINUX, t_info.required_cpu);
                VM_Attach(new_vm, m_id);
                Compute_Linux.push_back({new_vm, FindRemainingExecTime(new_vm), FindRemainingAvailMem(new_vm)});
                vms.push_back(new_vm);
                VM_AddTask(new_vm, task_id, HIGH_PRIORITY);
                return;
            }
        }

        for (unsigned i = 0; i < Compute_Linuxrt.size(); i++) {
            VMResourcePair vm_pair = Compute_Linuxrt[i];
            VMInfo_t vm_info = VM_GetInfo(vm_pair.vm_id);
            MachineId_t m_id = vm_info.machine_id;
            if (vm_info.active_tasks.size() == 0 && Machine_GetInfo(m_id).cpu == t_info.required_cpu) {
                Compute_Linuxrt.erase(remove_if(Compute_Linuxrt.begin(), Compute_Linuxrt.end(),
                        [vm_pair](const VMResourcePair& s) {
                            return s.vm_id == vm_pair.vm_id;
                        }), Compute_Linuxrt.end());
                vms.erase(remove(vms.begin(), vms.end(), vm_pair.vm_id), vms.end());
                VM_Shutdown(vm_info.vm_id);
                VMId_t new_vm = VM_Create(LINUX, t_info.required_cpu);
                VM_Attach(new_vm, m_id);
                Compute_Linux.push_back({new_vm, FindRemainingExecTime(new_vm), FindRemainingAvailMem(new_vm)});
                vms.push_back(new_vm);
                VM_AddTask(new_vm, task_id, HIGH_PRIORITY);
                return;
            }
        }

        for (unsigned i = 0; i < Compute_Aix.size(); i++) {
            VMResourcePair vm_pair = Compute_Aix[i];
            VMInfo_t vm_info = VM_GetInfo(vm_pair.vm_id);
            MachineId_t m_id = vm_info.machine_id;
            if (vm_info.active_tasks.size() == 0 && Machine_GetInfo(m_id).cpu == t_info.required_cpu) {
                Compute_Aix.erase(remove_if(Compute_Aix.begin(), Compute_Aix.end(),
                        [vm_pair](const VMResourcePair& s) {
                            return s.vm_id == vm_pair.vm_id;
                        }), Compute_Aix.end());
                vms.erase(remove(vms.begin(), vms.end(), vm_pair.vm_id), vms.end());
                VM_Shutdown(vm_info.vm_id);
                VMId_t new_vm = VM_Create(LINUX, t_info.required_cpu);
                VM_Attach(new_vm, m_id);
                Compute_Linux.push_back({new_vm, FindRemainingExecTime(new_vm), FindRemainingAvailMem(new_vm)});
                vms.push_back(new_vm);
                VM_AddTask(new_vm, task_id, HIGH_PRIORITY);
                return;
            }
        }

        // if that still doesn't work then start pulling from IO VMs
        for (unsigned i = 0; i < IO_Win.size(); i++) {
            VMResourcePair vm_pair = IO_Win[i];
            VMInfo_t vm_info = VM_GetInfo(vm_pair.vm_id);
            MachineId_t m_id = vm_info.machine_id;
            if (vm_info.active_tasks.size() == 0 && Machine_GetInfo(m_id).cpu == t_info.required_cpu) {
                IO_Win.erase(remove_if(IO_Win.begin(), IO_Win.end(),
                        [vm_pair](const VMResourcePair& s) {
                            return s.vm_id == vm_pair.vm_id;
                        }), IO_Win.end());
                vms.erase(remove(vms.begin(), vms.end(), vm_pair.vm_id), vms.end());
                VM_Shutdown(vm_info.vm_id);
                VMId_t new_vm = VM_Create(LINUX, t_info.required_cpu);
                VM_Attach(new_vm, m_id);
                Compute_Linux.push_back({new_vm, FindRemainingExecTime(new_vm), FindRemainingAvailMem(new_vm)});
                vms.push_back(new_vm);
                VM_AddTask(new_vm, task_id, HIGH_PRIORITY);
                return;
            }
        }

        for (unsigned i = 0; i < IO_Linuxrt.size(); i++) {
            VMResourcePair vm_pair = IO_Linuxrt[i];
            VMInfo_t vm_info = VM_GetInfo(vm_pair.vm_id);
            MachineId_t m_id = vm_info.machine_id;
            if (vm_info.active_tasks.size() == 0 && Machine_GetInfo(m_id).cpu == t_info.required_cpu) {
                IO_Linuxrt.erase(remove_if(IO_Linuxrt.begin(), IO_Linuxrt.end(),
                        [vm_pair](const VMResourcePair& s) {
                            return s.vm_id == vm_pair.vm_id;
                        }), IO_Linuxrt.end());
                vms.erase(remove(vms.begin(), vms.end(), vm_pair.vm_id), vms.end());
                VM_Shutdown(vm_info.vm_id);
                VMId_t new_vm = VM_Create(LINUX, t_info.required_cpu);
                VM_Attach(new_vm, m_id);
                Compute_Linux.push_back({new_vm, FindRemainingExecTime(new_vm), FindRemainingAvailMem(new_vm)});
                vms.push_back(new_vm);
                VM_AddTask(new_vm, task_id, HIGH_PRIORITY);
                return;
            }
        }

        for (unsigned i = 0; i < IO_Aix.size(); i++) {
            VMResourcePair vm_pair = IO_Aix[i];
            VMInfo_t vm_info = VM_GetInfo(vm_pair.vm_id);
            MachineId_t m_id = vm_info.machine_id;
            if (vm_info.active_tasks.size() == 0 && Machine_GetInfo(m_id).cpu == t_info.required_cpu) {
                IO_Aix.erase(remove_if(IO_Aix.begin(), IO_Aix.end(),
                        [vm_pair](const VMResourcePair& s) {
                            return s.vm_id == vm_pair.vm_id;
                        }), IO_Aix.end());
                vms.erase(remove(vms.begin(), vms.end(), vm_pair.vm_id), vms.end());
                VM_Shutdown(vm_info.vm_id);
                VMId_t new_vm = VM_Create(LINUX, t_info.required_cpu);
                VM_Attach(new_vm, m_id);
                Compute_Linux.push_back({new_vm, FindRemainingExecTime(new_vm), FindRemainingAvailMem(new_vm)});
                vms.push_back(new_vm);
                VM_AddTask(new_vm, task_id, HIGH_PRIORITY);
                return;
            }
        }
    }
    else {
        // pull from idle memory VMs
        for (unsigned i = 0; i < IO_Win.size(); i++) {
            VMResourcePair vm_pair = IO_Win[i];
            VMInfo_t vm_info = VM_GetInfo(vm_pair.vm_id);
            MachineId_t m_id = vm_info.machine_id;
            if (vm_info.active_tasks.size() == 0 && Machine_GetInfo(m_id).cpu == t_info.required_cpu) {
                IO_Win.erase(remove_if(IO_Win.begin(), IO_Win.end(),
                        [vm_pair](const VMResourcePair& s) {
                            return s.vm_id == vm_pair.vm_id;
                        }), IO_Win.end());
                vms.erase(remove(vms.begin(), vms.end(), vm_pair.vm_id), vms.end());
                VM_Shutdown(vm_info.vm_id);
                VMId_t new_vm = VM_Create(LINUX, t_info.required_cpu);
                VM_Attach(new_vm, m_id);
                IO_Linux.push_back({new_vm, FindRemainingExecTime(new_vm), FindRemainingAvailMem(new_vm)});
                vms.push_back(new_vm);
                VM_AddTask(new_vm, task_id, HIGH_PRIORITY);
                return;
            }
        }

        for (unsigned i = 0; i < IO_Linuxrt.size(); i++) {
            VMResourcePair vm_pair = IO_Linuxrt[i];
            VMInfo_t vm_info = VM_GetInfo(vm_pair.vm_id);
            MachineId_t m_id = vm_info.machine_id;
            if (vm_info.active_tasks.size() == 0 && Machine_GetInfo(m_id).cpu == t_info.required_cpu) {
                IO_Linuxrt.erase(remove_if(IO_Linuxrt.begin(), IO_Linuxrt.end(),
                        [vm_pair](const VMResourcePair& s) {
                            return s.vm_id == vm_pair.vm_id;
                        }), IO_Linuxrt.end());
                vms.erase(remove(vms.begin(), vms.end(), vm_pair.vm_id), vms.end());
                VM_Shutdown(vm_info.vm_id);
                VMId_t new_vm = VM_Create(LINUX, t_info.required_cpu);
                VM_Attach(new_vm, m_id);
                IO_Linux.push_back({new_vm, FindRemainingExecTime(new_vm), FindRemainingAvailMem(new_vm)});
                vms.push_back(new_vm);
                VM_AddTask(new_vm, task_id, HIGH_PRIORITY);
                return;
            }
        }

        for (unsigned i = 0; i < IO_Aix.size(); i++) {
            VMResourcePair vm_pair = IO_Aix[i];
            VMInfo_t vm_info = VM_GetInfo(vm_pair.vm_id);
            MachineId_t m_id = vm_info.machine_id;
            if (vm_info.active_tasks.size() == 0 && Machine_GetInfo(m_id).cpu == t_info.required_cpu) {
                IO_Aix.erase(remove_if(IO_Aix.begin(), IO_Aix.end(),
                        [vm_pair](const VMResourcePair& s) {
                            return s.vm_id == vm_pair.vm_id;
                        }), IO_Aix.end());
                vms.erase(remove(vms.begin(), vms.end(), vm_pair.vm_id), vms.end());
                VM_Shutdown(vm_info.vm_id);
                VMId_t new_vm = VM_Create(LINUX, t_info.required_cpu);
                VM_Attach(new_vm, m_id);
                IO_Linux.push_back({new_vm, FindRemainingExecTime(new_vm), FindRemainingAvailMem(new_vm)});
                vms.push_back(new_vm);
                VM_AddTask(new_vm, task_id, HIGH_PRIORITY);
                return;
            }
        }

        // if that still doesn't work then start pulling from compute VMs
        for (unsigned i = 0; i < Compute_Win.size(); i++) {
            VMResourcePair vm_pair = Compute_Win[i];
            VMInfo_t vm_info = VM_GetInfo(vm_pair.vm_id);
            MachineId_t m_id = vm_info.machine_id;
            if (vm_info.active_tasks.size() == 0 && Machine_GetInfo(m_id).cpu == t_info.required_cpu) {
                Compute_Win.erase(remove_if(Compute_Win.begin(), Compute_Win.end(),
                        [vm_pair](const VMResourcePair& s) {
                            return s.vm_id == vm_pair.vm_id;
                        }), Compute_Win.end());
                vms.erase(remove(vms.begin(), vms.end(), vm_pair.vm_id), vms.end());
                VM_Shutdown(vm_info.vm_id);
                VMId_t new_vm = VM_Create(LINUX, t_info.required_cpu);
                VM_Attach(new_vm, m_id);
                IO_Linux.push_back({new_vm, FindRemainingExecTime(new_vm), FindRemainingAvailMem(new_vm)});
                vms.push_back(new_vm);
                VM_AddTask(new_vm, task_id, HIGH_PRIORITY);
                return;
            }
        }

        for (unsigned i = 0; i < Compute_Linuxrt.size(); i++) {
            VMResourcePair vm_pair = Compute_Linuxrt[i];
            VMInfo_t vm_info = VM_GetInfo(vm_pair.vm_id);
            MachineId_t m_id = vm_info.machine_id;
            if (vm_info.active_tasks.size() == 0 && Machine_GetInfo(m_id).cpu == t_info.required_cpu) {
                Compute_Linuxrt.erase(remove_if(Compute_Linuxrt.begin(), Compute_Linuxrt.end(),
                        [vm_pair](const VMResourcePair& s) {
                            return s.vm_id == vm_pair.vm_id;
                        }), Compute_Linuxrt.end());
                vms.erase(remove(vms.begin(), vms.end(), vm_pair.vm_id), vms.end());
                VM_Shutdown(vm_info.vm_id);
                VMId_t new_vm = VM_Create(LINUX, t_info.required_cpu);
                VM_Attach(new_vm, m_id);
                IO_Linux.push_back({new_vm, FindRemainingExecTime(new_vm), FindRemainingAvailMem(new_vm)});
                vms.push_back(new_vm);
                VM_AddTask(new_vm, task_id, HIGH_PRIORITY);
                return;
            }
        }

        for (unsigned i = 0; i < Compute_Aix.size(); i++) {
            VMResourcePair vm_pair = Compute_Aix[i];
            VMInfo_t vm_info = VM_GetInfo(vm_pair.vm_id);
            MachineId_t m_id = vm_info.machine_id;
            if (vm_info.active_tasks.size() == 0 && Machine_GetInfo(m_id).cpu == t_info.required_cpu) {
                Compute_Aix.erase(remove_if(Compute_Aix.begin(), Compute_Aix.end(),
                        [vm_pair](const VMResourcePair& s) {
                            return s.vm_id == vm_pair.vm_id;
                        }), Compute_Aix.end());
                vms.erase(remove(vms.begin(), vms.end(), vm_pair.vm_id), vms.end());
                VM_Shutdown(vm_info.vm_id);
                VMId_t new_vm = VM_Create(LINUX, t_info.required_cpu);
                VM_Attach(new_vm, m_id);
                IO_Linux.push_back({new_vm, FindRemainingExecTime(new_vm), FindRemainingAvailMem(new_vm)});
                vms.push_back(new_vm);
                VM_AddTask(new_vm, task_id, HIGH_PRIORITY);
                return;
            }
        }
    }
}


void Scheduler::AllocateNewWinVM(TaskId_t task_id, TaskInfo_t t_info, bool compute_task) {
    if (compute_task) {
        for (unsigned i = 0; i < Compute_Linuxrt.size(); i++) {
            VMResourcePair vm_pair = Compute_Linuxrt[i];
            VMInfo_t vm_info = VM_GetInfo(vm_pair.vm_id);
            MachineId_t m_id = vm_info.machine_id;
            if (vm_info.active_tasks.size() == 0 && Machine_GetInfo(m_id).cpu == t_info.required_cpu) {
                Compute_Linuxrt.erase(remove_if(Compute_Linuxrt.begin(), Compute_Linuxrt.end(),
                        [vm_pair](const VMResourcePair& s) {
                            return s.vm_id == vm_pair.vm_id;
                        }), Compute_Linuxrt.end());
                vms.erase(remove(vms.begin(), vms.end(), vm_pair.vm_id), vms.end());
                VM_Shutdown(vm_info.vm_id);
                VMId_t new_vm = VM_Create(LINUX, t_info.required_cpu);
                VM_Attach(new_vm, m_id);
                Compute_Win.push_back({new_vm, FindRemainingExecTime(new_vm), FindRemainingAvailMem(new_vm)});
                vms.push_back(new_vm);
                VM_AddTask(new_vm, task_id, HIGH_PRIORITY);
                return;
            }
        }
        for (unsigned i = 0; i < Compute_Linux.size(); i++) {
            VMResourcePair vm_pair = Compute_Linux[i];
            VMInfo_t vm_info = VM_GetInfo(vm_pair.vm_id);
            MachineId_t m_id = vm_info.machine_id;
            if (vm_info.active_tasks.size() == 0 && Machine_GetInfo(m_id).cpu == t_info.required_cpu) {
                Compute_Linux.erase(remove_if(Compute_Linux.begin(), Compute_Linux.end(),
                            [vm_pair](const VMResourcePair& s) {
                            return s.vm_id == vm_pair.vm_id;
                            }), Compute_Linux.end());
                vms.erase(remove(vms.begin(), vms.end(), vm_pair.vm_id), vms.end());
                VM_Shutdown(vm_info.vm_id);
                VMId_t new_vm = VM_Create(WIN, t_info.required_cpu);
                VM_Attach(new_vm, m_id);
                Compute_Win.push_back({new_vm, FindRemainingExecTime(new_vm), FindRemainingAvailMem(new_vm)});
                vms.push_back(new_vm);
                VM_AddTask(new_vm, task_id, HIGH_PRIORITY);
                return;
            }
        } 

        // if that still doesn't work then start pulling from IO vms
        for (unsigned i = 0; i < IO_Linuxrt.size(); i++) {
            VMResourcePair vm_pair = IO_Linuxrt[i];
            VMInfo_t vm_info = VM_GetInfo(vm_pair.vm_id);
            MachineId_t m_id = vm_info.machine_id;
            if (vm_info.active_tasks.size() == 0 && Machine_GetInfo(m_id).cpu == t_info.required_cpu) {
                IO_Linuxrt.erase(remove_if(IO_Linuxrt.begin(), IO_Linuxrt.end(),
                        [vm_pair](const VMResourcePair& s) {
                            return s.vm_id == vm_pair.vm_id;
                        }), IO_Linuxrt.end());
                vms.erase(remove(vms.begin(), vms.end(), vm_pair.vm_id), vms.end());
                VM_Shutdown(vm_info.vm_id);
                VMId_t new_vm = VM_Create(LINUX, t_info.required_cpu);
                VM_Attach(new_vm, m_id);
                Compute_Win.push_back({new_vm, FindRemainingExecTime(new_vm), FindRemainingAvailMem(new_vm)});
                vms.push_back(new_vm);
                VM_AddTask(new_vm, task_id, HIGH_PRIORITY);
                return;
            }
        }
        for (unsigned i = 0; i < IO_Linux.size(); i++) {
            VMResourcePair vm_pair = IO_Linux[i];
            VMInfo_t vm_info = VM_GetInfo(vm_pair.vm_id);
            MachineId_t m_id = vm_info.machine_id;
            if (vm_info.active_tasks.size() == 0 && Machine_GetInfo(m_id).cpu == t_info.required_cpu) {
                IO_Linux.erase(remove_if(IO_Linux.begin(), IO_Linux.end(),
                            [vm_pair](const VMResourcePair& s) {
                            return s.vm_id == vm_pair.vm_id;
                            }), IO_Linux.end());
                vms.erase(remove(vms.begin(), vms.end(), vm_pair.vm_id), vms.end());
                VM_Shutdown(vm_info.vm_id);
                VMId_t new_vm = VM_Create(WIN, t_info.required_cpu);
                VM_Attach(new_vm, m_id);
                Compute_Win.push_back({new_vm, FindRemainingExecTime(new_vm), FindRemainingAvailMem(new_vm)});
                vms.push_back(new_vm);
                VM_AddTask(new_vm, task_id, HIGH_PRIORITY);
                return;
            }
        }
    }
    else {
        for (unsigned i = 0; i < IO_Linuxrt.size(); i++) {
            VMResourcePair vm_pair = IO_Linuxrt[i];
            VMInfo_t vm_info = VM_GetInfo(vm_pair.vm_id);
            MachineId_t m_id = vm_info.machine_id;
            if (vm_info.active_tasks.size() == 0 && Machine_GetInfo(m_id).cpu == t_info.required_cpu) {
                IO_Linuxrt.erase(remove_if(IO_Linuxrt.begin(), IO_Linuxrt.end(),
                        [vm_pair](const VMResourcePair& s) {
                            return s.vm_id == vm_pair.vm_id;
                        }), IO_Linuxrt.end());
                vms.erase(remove(vms.begin(), vms.end(), vm_pair.vm_id), vms.end());
                VM_Shutdown(vm_info.vm_id);
                VMId_t new_vm = VM_Create(WIN, t_info.required_cpu);
                VM_Attach(new_vm, m_id);
                IO_Win.push_back({new_vm, FindRemainingExecTime(new_vm), FindRemainingAvailMem(new_vm)});
                vms.push_back(new_vm);
                VM_AddTask(new_vm, task_id, HIGH_PRIORITY);
                return;
            }
        }
        for (unsigned i = 0; i < IO_Linux.size(); i++) {
            VMResourcePair vm_pair = IO_Linux[i];
            VMInfo_t vm_info = VM_GetInfo(vm_pair.vm_id);
            MachineId_t m_id = vm_info.machine_id;
            if (vm_info.active_tasks.size() == 0 && Machine_GetInfo(m_id).cpu == t_info.required_cpu) {
                IO_Linux.erase(remove_if(IO_Linux.begin(), IO_Linux.end(),
                            [vm_pair](const VMResourcePair& s) {
                            return s.vm_id == vm_pair.vm_id;
                            }), IO_Linux.end());
                vms.erase(remove(vms.begin(), vms.end(), vm_pair.vm_id), vms.end());
                VM_Shutdown(vm_info.vm_id);
                VMId_t new_vm = VM_Create(WIN, t_info.required_cpu);
                VM_Attach(new_vm, m_id);
                IO_Win.push_back({new_vm, FindRemainingExecTime(new_vm), FindRemainingAvailMem(new_vm)});
                vms.push_back(new_vm);
                VM_AddTask(new_vm, task_id, HIGH_PRIORITY);
                return;
            }
        }
        
        // if that doesn't work then start pulling form compute pool
        for (unsigned i = 0; i < Compute_Linuxrt.size(); i++) {
            VMResourcePair vm_pair = Compute_Linuxrt[i];
            VMInfo_t vm_info = VM_GetInfo(vm_pair.vm_id);
            MachineId_t m_id = vm_info.machine_id;
            if (vm_info.active_tasks.size() == 0 && Machine_GetInfo(m_id).cpu == t_info.required_cpu) {
                Compute_Linuxrt.erase(remove_if(Compute_Linuxrt.begin(), Compute_Linuxrt.end(),
                        [vm_pair](const VMResourcePair& s) {
                            return s.vm_id == vm_pair.vm_id;
                        }), Compute_Linuxrt.end());
                vms.erase(remove(vms.begin(), vms.end(), vm_pair.vm_id), vms.end());
                VM_Shutdown(vm_info.vm_id);
                VMId_t new_vm = VM_Create(LINUX, t_info.required_cpu);
                VM_Attach(new_vm, m_id);
                IO_Win.push_back({new_vm, FindRemainingExecTime(new_vm), FindRemainingAvailMem(new_vm)});
                vms.push_back(new_vm);
                VM_AddTask(new_vm, task_id, HIGH_PRIORITY);
                return;
            }
        }
        for (unsigned i = 0; i < Compute_Linux.size(); i++) {
            VMResourcePair vm_pair = Compute_Linux[i];
            VMInfo_t vm_info = VM_GetInfo(vm_pair.vm_id);
            MachineId_t m_id = vm_info.machine_id;
            if (vm_info.active_tasks.size() == 0 && Machine_GetInfo(m_id).cpu == t_info.required_cpu) {
                Compute_Linux.erase(remove_if(Compute_Linux.begin(), Compute_Linux.end(),
                            [vm_pair](const VMResourcePair& s) {
                            return s.vm_id == vm_pair.vm_id;
                            }), Compute_Linux.end());
                vms.erase(remove(vms.begin(), vms.end(), vm_pair.vm_id), vms.end());
                VM_Shutdown(vm_info.vm_id);
                VMId_t new_vm = VM_Create(WIN, t_info.required_cpu);
                VM_Attach(new_vm, m_id);
                IO_Win.push_back({new_vm, FindRemainingExecTime(new_vm), FindRemainingAvailMem(new_vm)});
                vms.push_back(new_vm);
                VM_AddTask(new_vm, task_id, HIGH_PRIORITY);
                return;
            }
        } 
    }

}

void Scheduler::AllocateNewLinuxRTVM(TaskId_t task_id, TaskInfo_t t_info, bool compute_task) {

}

void Scheduler::AllocateNewAixVM(TaskId_t task_id, TaskInfo_t t_info, bool compute_task) {

}


void Scheduler::Init() {
    SimOutput("Scheduler::Init(): Total number of machines is " + to_string(Machine_GetTotal()), 3);
    SimOutput("Scheduler::Init(): Initializing scheduler", 1);

    unsigned numARM = 0;
    unsigned numRISCV = 0;
    unsigned numPOWER = 0;
    unsigned numX86 = 0;

    /* these data structures will simply just be local to this function 
       they will help us in initializing an optimal start and management
       of all our resources.
    */
    vector<MachineId_t> ArmMachines;
    vector<MachineId_t> PowerMachines;
    vector<MachineId_t> RiscvMachines;
    vector<MachineId_t> X86Machines;

    /* classify different types of machines to match the templates to */
    vector<MachineId_t> ComputeMachines;
    vector<MachineId_t> MemoryMachines;
    vector<MachineId_t> BalancedMachines;

    unsigned total_machines = Machine_GetTotal();

    maxNumCPUS = 0;
    maxMemory = 0;
    maxMIPS = 0;

    // go through all machines to find a maxNumCPUs and maxMemory metric
    // to perform ratio calculations
    for (unsigned i = 0; i < total_machines; i++) {
        MachineInfo_t m_info = Machine_GetInfo(MachineId_t(i));
        if (m_info.num_cpus > maxNumCPUS) {
            maxNumCPUS = m_info.num_cpus;
        }
        if (m_info.memory_size > maxMemory) {
            maxMemory = m_info.memory_size;
        }
        if (m_info.performance[0] > maxMIPS) {
            maxMIPS = m_info.performance[0];
        }
    }

    // process each machine to see how they match up to the max's
    for (unsigned i = 0; i < total_machines; i++) {
        MachineInfo_t m_info = Machine_GetInfo(MachineId_t(i));
        switch(m_info.cpu) {
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
        double ratio_cpu = m_info.num_cpus / maxNumCPUS;
        double ratio_memory = m_info.memory_size / maxMemory;
        double ratio_mips = m_info.performance[0] / maxMIPS;
        if (ratio_cpu > ratio_memory && ratio_cpu > 0.6) {
            // CPU resources dominate, so we deem this as a
            // compute intensive machine
            ComputeMachines.push_back(MachineId_t(i));
        }
        else if (ratio_mips > ratio_memory && ratio_mips > 0.6) {
            // CPU resources still odminate, so we still deem
            // this a compute intensive machine
            ComputeMachines.push_back(MachineId_t(i));
        }
        else if (ratio_memory > ratio_cpu && ratio_memory > 0.6) {
            // Memory resources dominate, so we deem this as a
            // memory intensive machine
            MemoryMachines.push_back(MachineId_t(i));
        }
        else {
            // this is a pretty balanced machine that can go to either
            BalancedMachines.push_back(MachineId_t(i));
        }
    }

    // statically initialize VMs per machine

    for (unsigned i = 0; i < numARM; i++) {
        VMId_t vm_created;
        // make sure to find machine type
        unsigned machine_type;
        if (find(ComputeMachines.begin(), ComputeMachines.end(), ArmMachines[i]) != ComputeMachines.end()) {
            // this is a compute intensive machine
            machine_type = 0;
        }
        else if (find(MemoryMachines.begin(), MemoryMachines.end(), ArmMachines[i]) != MemoryMachines.end()) {
            // this is a memory intensive machine
            machine_type = 1;
        }
        else {
            // this has to be a balanced machine
            machine_type = 2;
        }
        // now actually do the diff VM initializations
        if (i >= 0 && i < numARM/2) {
            vm_created = VM_Create(WIN, ARM);
            VM_Attach(vm_created, ArmMachines[i]);
            Time_t remainingtime = FindRemainingExecTime(vm_created);
            unsigned remainingmem = FindRemainingAvailMem(vm_created);
            if (machine_type == 0) {
                Compute_Win.push_back({vm_created, remainingtime, remainingmem});
            }
            else if (machine_type == 1) {
                IO_Win.push_back({vm_created, remainingtime, remainingmem});
            }
            else {
                Compute_Win.size() > IO_Win.size() ? 
                    IO_Win.push_back({vm_created, remainingtime, remainingmem}) 
                        : Compute_Win.push_back({vm_created, remainingtime, remainingmem});
            }
        }
        else if (i == numARM/2 || i == (numARM/2 + 1)) {
            vm_created = VM_Create(LINUX_RT, ARM);
            VM_Attach(vm_created, ArmMachines[i]);
            Time_t remainingtime = FindRemainingExecTime(vm_created);
            unsigned remainingmem = FindRemainingAvailMem(vm_created);
            if (machine_type == 0) {
                Compute_Linuxrt.push_back({vm_created, remainingtime, remainingmem});
            }
            else if (machine_type == 1) {
                IO_Linuxrt.push_back({vm_created, remainingtime, remainingmem});
            }
            else {
                Compute_Linuxrt.size() > IO_Linuxrt.size() ? 
                    IO_Linuxrt.push_back({vm_created, remainingtime, remainingmem}) 
                        : Compute_Linuxrt.push_back({vm_created, remainingtime, remainingmem});
            }
        }
        else {
            vm_created = VM_Create(LINUX, ARM);
            VM_Attach(vm_created, ArmMachines[i]);
            Time_t remainingtime = FindRemainingExecTime(vm_created);
            unsigned remainingmem = FindRemainingAvailMem(vm_created);
            if (machine_type == 0) {
                Compute_Linux.push_back({vm_created, remainingtime, remainingmem});
            }
            else if (machine_type == 1) {
                IO_Linux.push_back({vm_created, remainingtime, remainingmem});
            }
            else {
                Compute_Linux.size() > IO_Linux.size() ? 
                    IO_Linux.push_back({vm_created, remainingtime, remainingmem}) 
                        : Compute_Linux.push_back({vm_created, remainingtime, remainingmem});
            }
         }
         vms.push_back(vm_created);
    }

    for (unsigned i = 0; i < numRISCV; i++) {
        VMId_t vm_created;
        // make sure to find machine type
        unsigned machine_type;
        if (find(ComputeMachines.begin(), ComputeMachines.end(), RiscvMachines[i]) != ComputeMachines.end()) {
            // this is a compute intensive machine
            machine_type = 0;
        }
        else if (find(MemoryMachines.begin(), MemoryMachines.end(), RiscvMachines[i]) != MemoryMachines.end()) {
            // this is a memory intensive machine
            machine_type = 1;
        }
        else {
            // this has to be a balanced machine
            machine_type = 2;
        }
        
        // now actually do the diff VM initializations
        if (i < numRISCV/2) {
            // initialize LINUX machine
            vm_created = VM_Create(LINUX, RISCV);
            VM_Attach(vm_created, RiscvMachines[i]);
            Time_t remainingtime = FindRemainingExecTime(vm_created);
            unsigned remainingmem = FindRemainingAvailMem(vm_created);
            if (machine_type == 0) {
                Compute_Linux.push_back({vm_created, remainingtime, remainingmem});
            }
            else if (machine_type == 1) {
                IO_Linux.push_back({vm_created, remainingtime, remainingmem});
            }
            else {
                Compute_Linux.size() > IO_Linux.size() ? 
                    IO_Linux.push_back({vm_created, remainingtime, remainingmem}) 
                        : Compute_Linux.push_back({vm_created, remainingtime, remainingmem});
            }
        }
        else {
            // initialize other machines 
            vm_created = VM_Create(LINUX_RT, RISCV);
            VM_Attach(vm_created, RiscvMachines[i]);
            Time_t remainingtime = FindRemainingExecTime(vm_created);
            unsigned remainingmem = FindRemainingAvailMem(vm_created);
            if (machine_type == 0) {
                Compute_Linuxrt.push_back({vm_created, remainingtime, remainingmem});
            }
            else if (machine_type == 1) {
                IO_Linuxrt.push_back({vm_created, remainingtime, remainingmem});
            }
            else {
                Compute_Linuxrt.size() > IO_Linuxrt.size() ? 
                    IO_Linuxrt.push_back({vm_created, remainingtime, remainingmem}) 
                        : Compute_Linuxrt.push_back({vm_created, remainingtime, remainingmem});
            }
        }
        vms.push_back(vm_created);
    }

    for (unsigned i = 0; i < numPOWER; i++) {
        VMId_t vm_created;
        // make sure to find machine type
        unsigned machine_type;
        if (find(ComputeMachines.begin(), ComputeMachines.end(), PowerMachines[i]) != ComputeMachines.end()) {
            // this is a compute intensive machine
            machine_type = 0;
        }
        else if (find(MemoryMachines.begin(), MemoryMachines.end(), PowerMachines[i]) != MemoryMachines.end()) {
            // this is a memory intensive machine
            machine_type = 1;
        }
        else {
            // this has to be a balanced machine
            machine_type = 2;
        }

        // now actually do the diff VM initializations
        if (i < numPOWER/2) {
            // initialize AIX machines
            vm_created = VM_Create(AIX, POWER);
            VM_Attach(vm_created, PowerMachines[i]);
            Time_t remainingtime = FindRemainingExecTime(vm_created);
            unsigned remainingmem = FindRemainingAvailMem(vm_created);
            if (machine_type == 0) {
                Compute_Aix.push_back({vm_created, remainingtime, remainingmem});
            }
            else if (machine_type == 1) {
                IO_Aix.push_back({vm_created, remainingtime, remainingmem});
            }
            else {
                Compute_Aix.size() > IO_Aix.size() ? 
                    IO_Aix.push_back({vm_created, remainingtime, remainingmem}) 
                        : Compute_Aix.push_back({vm_created, remainingtime, remainingmem});
            }
        }
        else if (i == numPOWER/2) {
            vm_created = VM_Create(LINUX_RT, POWER);
            VM_Attach(vm_created, PowerMachines[i]);
            Time_t remainingtime = FindRemainingExecTime(vm_created);
            unsigned remainingmem = FindRemainingAvailMem(vm_created);
            if (machine_type == 0) {
                Compute_Linuxrt.push_back({vm_created, remainingtime, remainingmem});
            }
            else if (machine_type == 1) {
                IO_Linuxrt.push_back({vm_created, remainingtime, remainingmem});
            }
            else {
                Compute_Linuxrt.size() > IO_Linuxrt.size() ? 
                    IO_Linuxrt.push_back({vm_created, remainingtime, remainingmem}) 
                        : Compute_Linuxrt.push_back({vm_created, remainingtime, remainingmem});
            }
        }
        else {
            vm_created = VM_Create(LINUX, POWER);
            VM_Attach(vm_created, PowerMachines[i]);
            Time_t remainingtime = FindRemainingExecTime(vm_created);
            unsigned remainingmem = FindRemainingAvailMem(vm_created);
            if (machine_type == 0) {
                Compute_Linux.push_back({vm_created, remainingtime, remainingmem});
            }
            else if (machine_type == 1) {
                IO_Linux.push_back({vm_created, remainingtime, remainingmem});
            }
            else {
                Compute_Linux.size() > IO_Linux.size() ? 
                    IO_Linux.push_back({vm_created, remainingtime, remainingmem}) 
                        : Compute_Linux.push_back({vm_created, remainingtime, remainingmem});
            }
        }
        vms.push_back(vm_created);
    }

    for (unsigned i = 0; i < numX86; i++) {
        VMId_t vm_created;
        // make sure to find machine type
        unsigned machine_type;
        if (find(ComputeMachines.begin(), ComputeMachines.end(), X86Machines[i]) != ComputeMachines.end()) {
            // this is a compute intensive machine
            machine_type = 0;
        }
        else if (find(MemoryMachines.begin(), MemoryMachines.end(), X86Machines[i]) != MemoryMachines.end()) {
            // this is a memory intensive machine
            machine_type = 1;
        }
        else {
            // this has to be a balanced machine
            machine_type = 2;
        }
        
        // now actually do the diff VM initializations
        if (i >= 0 && i < numX86/2) {
            // initialize LINUX machine
            vm_created = VM_Create(LINUX, X86);
            VM_Attach(vm_created, X86Machines[i]);
            Time_t remainingtime = FindRemainingExecTime(vm_created);
            unsigned remainingmem = FindRemainingAvailMem(vm_created);
            if (machine_type == 0) {
                Compute_Linux.push_back({vm_created, remainingtime, remainingmem});
            }
            else if (machine_type == 1) {
                IO_Linux.push_back({vm_created, remainingtime, remainingmem});
            }
            else {
                Compute_Linux.size() > IO_Linux.size() ? 
                    IO_Linux.push_back({vm_created, remainingtime, remainingmem}) 
                        : Compute_Linux.push_back({vm_created, remainingtime, remainingmem});
            }
        }
        else if (i == numX86/2 || i == (numX86/2 + 1)) {
            vm_created = VM_Create(LINUX_RT, X86);
            VM_Attach(vm_created, X86Machines[i]);
            Time_t remainingtime = FindRemainingExecTime(vm_created);
            unsigned remainingmem = FindRemainingAvailMem(vm_created);
            if (machine_type == 0) {
                Compute_Linuxrt.push_back({vm_created, remainingtime, remainingmem});
            }
            else if (machine_type == 1) {
                IO_Linuxrt.push_back({vm_created, remainingtime, remainingmem});
            }
            else {
                Compute_Linuxrt.size() > IO_Linuxrt.size() ? 
                    IO_Linuxrt.push_back({vm_created, remainingtime, remainingmem}) 
                        : Compute_Linuxrt.push_back({vm_created, remainingtime, remainingmem});
            }
        }
        else {
            vm_created = VM_Create(WIN, X86);
            VM_Attach(vm_created, X86Machines[i]);
            Time_t remainingtime = FindRemainingExecTime(vm_created);
            unsigned remainingmem = FindRemainingAvailMem(vm_created);
            if (machine_type == 0) {
                Compute_Win.push_back({vm_created, remainingtime, remainingmem});
            }
            else if (machine_type == 1) {
                IO_Win.push_back({vm_created, remainingtime, remainingmem});
            }
            else {
                Compute_Win.size() > IO_Win.size() ? 
                    IO_Win.push_back({vm_created, remainingtime, remainingmem}) 
                        : Compute_Win.push_back({vm_created, remainingtime, remainingmem});
            }
        }
        vms.push_back(vm_created);
    }

    // debugging statements
    cout << "number of compute machines found: " << ComputeMachines.size() << endl;
    cout << "number of io machines found: " << MemoryMachines.size() << endl;
    cout << "number of balanced machines found: " << BalancedMachines.size() << endl;

    cout << "number of compute linux vms made: " << Compute_Linux.size() << endl;
    cout << "number of compute linuxrt vms made: " << Compute_Linuxrt.size() << endl;
    cout << "number of compute win vms made: " << Compute_Win.size() << endl;
    cout << "number of compute aix vms made: " << Compute_Aix.size() << endl;

    cout << "number of io linux vms made: " << IO_Linux.size() << endl;
    cout << "number of io linuxrt vms made: " << IO_Linuxrt.size() << endl;
    cout << "number of io win vms made: " << IO_Win.size() << endl;
    cout << "number of io aix vms made: " << IO_Aix.size() << endl;
}

void Scheduler::MigrationComplete(Time_t time, VMId_t vm_id) {
    // Update your data structure. The VM now can receive new tasks
}

void Scheduler::NewTask(Time_t now, TaskId_t task_id) {
    task_template new_template = TemplateExtraction(task_id, now);
    TaskInfo_t t_info = GetTaskInfo(task_id);

    vector<VMResourcePair> vm_sorted_resource;
    vector<VMResourcePair> adjusted_sorted_resource;

    // only looking through the pool of vms that match this task's required vm type and task type
   if (new_template.compute_task) {
    switch(t_info.required_vm) {
        case LINUX:
            for (VMResourcePair vm_pair: Compute_Linux) {
                Time_t pending_execution_time = FindRemainingExecTime(vm_pair.vm_id);
                unsigned available_mem = FindRemainingAvailMem(vm_pair.vm_id);
                vm_sorted_resource.push_back({vm_pair.vm_id, pending_execution_time, available_mem});
            }
            break;
        case LINUX_RT:
            for (VMResourcePair vm_pair: Compute_Linuxrt) {
                Time_t pending_execution_time = FindRemainingExecTime(vm_pair.vm_id);
                unsigned available_mem = FindRemainingAvailMem(vm_pair.vm_id);
                vm_sorted_resource.push_back({vm_pair.vm_id, pending_execution_time, available_mem});
            }
            break;
        case WIN:
            for (VMResourcePair vm_pair: Compute_Win) {
                Time_t pending_execution_time = FindRemainingExecTime(vm_pair.vm_id);
                unsigned available_mem = FindRemainingAvailMem(vm_pair.vm_id);
                vm_sorted_resource.push_back({vm_pair.vm_id, pending_execution_time, available_mem});
            }
            break;
        case AIX:
            for (VMResourcePair vm_pair: Compute_Aix) {
                Time_t pending_execution_time = FindRemainingExecTime(vm_pair.vm_id);
                unsigned available_mem = FindRemainingAvailMem(vm_pair.vm_id);
                vm_sorted_resource.push_back({vm_pair.vm_id, pending_execution_time, available_mem});
            }
            break;
        default:
            break;
    }
    // sort based on ascending pending execution time
    sort(vm_sorted_resource.begin(), vm_sorted_resource.end(),
        [](const VMResourcePair& a, VMResourcePair& b){
            return a.pending_execution_time < b.pending_execution_time;
        });
    // now go through and adjust every vm on this list by its remaining resources and
    // re-sort based on pending execution time
    for (VMResourcePair vm_pair: vm_sorted_resource) {
        MachineInfo_t m_info = Machine_GetInfo(VM_GetInfo(vm_pair.vm_id).machine_id);
        Time_t adjusted_time = FindAdjustedExecTime(task_id, vm_pair.vm_id, vm_pair.pending_execution_time);
        unsigned adjusted_mem = FindAdjustedAvailMem(task_id, vm_pair.vm_id, m_info.memory_size - m_info.memory_used);
        adjusted_sorted_resource.push_back({vm_pair.vm_id, adjusted_time, adjusted_mem});
    }
    sort(adjusted_sorted_resource.begin(), adjusted_sorted_resource.end(),
        [](const VMResourcePair& a, VMResourcePair& b){
            return a.pending_execution_time < b.pending_execution_time;
        });
   }
   else {
    switch(t_info.required_vm) {
        case LINUX:
            for (VMResourcePair vm_pair: IO_Linux) {
                Time_t pending_execution_time = FindRemainingExecTime(vm_pair.vm_id);
                unsigned available_mem = FindRemainingAvailMem(vm_pair.vm_id);
                vm_sorted_resource.push_back({vm_pair.vm_id, pending_execution_time, available_mem});
            }
            break;
        case LINUX_RT:
            for (VMResourcePair vm_pair: IO_Linuxrt) {
                Time_t pending_execution_time = FindRemainingExecTime(vm_pair.vm_id);
                unsigned available_mem = FindRemainingAvailMem(vm_pair.vm_id);
                vm_sorted_resource.push_back({vm_pair.vm_id, pending_execution_time, available_mem});
            }
            break;
        case WIN:
            for (VMResourcePair vm_pair: IO_Win) {
                Time_t pending_execution_time = FindRemainingExecTime(vm_pair.vm_id);
                unsigned available_mem = FindRemainingAvailMem(vm_pair.vm_id);
                vm_sorted_resource.push_back({vm_pair.vm_id, pending_execution_time, available_mem});
            }
            break;
        case AIX:
            for (VMResourcePair vm_pair: IO_Aix) {
                Time_t pending_execution_time = FindRemainingExecTime(vm_pair.vm_id);
                unsigned available_mem = FindRemainingAvailMem(vm_pair.vm_id);
                vm_sorted_resource.push_back({vm_pair.vm_id, pending_execution_time, available_mem});
            }
            break;
        default:
            break;
    }
    // sort by descending available memory order
    sort(vm_sorted_resource.begin(), vm_sorted_resource.end(),
        [](const VMResourcePair& a, VMResourcePair& b){
            return a.avail_mem > b.avail_mem;
        });
    // now go through and adjust every vm on this list by its remaining resources and 
    // re-sort based on available memory remaining
    for (VMResourcePair vm_pair: vm_sorted_resource) {
        MachineInfo_t m_info = Machine_GetInfo(VM_GetInfo(vm_pair.vm_id).machine_id);
        Time_t adjusted_time = FindAdjustedExecTime(task_id, vm_pair.vm_id, vm_pair.pending_execution_time);
        unsigned adjusted_mem = FindAdjustedAvailMem(task_id, vm_pair.vm_id, m_info.memory_size - m_info.memory_used);
        adjusted_sorted_resource.push_back({vm_pair.vm_id, adjusted_time, adjusted_mem});
    }
    sort(adjusted_sorted_resource.begin(), adjusted_sorted_resource.end(),
        [](const VMResourcePair& a, VMResourcePair& b){
            return a.avail_mem > b.avail_mem;
        });
   }

    for (unsigned i = 0; i < adjusted_sorted_resource.size(); i++) {
        VMResourcePair vm_pair = adjusted_sorted_resource[i];
        VMId_t possible_vm = vm_pair.vm_id;
        MachineInfo_t m_info = Machine_GetInfo(VM_GetInfo(possible_vm).machine_id);
        if (m_info.cpu == t_info.required_cpu && vm_pair.avail_mem > 0) {
            VM_AddTask(possible_vm, task_id, HIGH_PRIORITY);
            return;
        }
    }

    // reaching here means all other VMs of this task's required VM type are overcommitted.
    // need to convert a compatible unused machine to a VM of this type instead.
    switch(t_info.required_vm) {
        case LINUX:
            AllocateNewLinuxVM(task_id, t_info, new_template.compute_task);
            break;
        case LINUX_RT:
            AllocateNewLinuxRTVM(task_id, t_info, new_template.compute_task);
            break;
        case WIN:
            AllocateNewWinVM(task_id, t_info, new_template.compute_task);
            break;
        case AIX:
            AllocateNewAixVM(task_id, t_info, new_template.compute_task);
            break;
        default:
            break;
    }


//    // now just choose the first vm that matches cpu description and (if possible) gpu
//    if (new_template.gpu_enabled) {
//     for (unsigned i = 0; i < adjusted_sorted_resource.size(); i++) {
//         VMResourcePair vm_pair = adjusted_sorted_resource[i];
//         VMId_t possible_vm = vm_pair.vm_id;
//         MachineInfo_t m_info = Machine_GetInfo(VM_GetInfo(possible_vm).machine_id);
//         if (m_info.cpu == t_info.required_cpu && vm_pair.avail_mem > 0) {
//             VM_AddTask(possible_vm, task_id, HIGH_PRIORITY);
//             return;
//         }
//     }
//    }
//    else {
//     for (unsigned i = 0; i < adjusted_sorted_resource.size(); i++) {
//         VMResourcePair vm_pair = adjusted_sorted_resource[i];
//         VMId_t possible_vm = vm_pair.vm_id;
//         MachineInfo_t m_info = Machine_GetInfo(VM_GetInfo(possible_vm).machine_id);
//         if (m_info.cpu == t_info.required_cpu && vm_pair.avail_mem > 0) {
//             VM_AddTask(possible_vm, task_id, HIGH_PRIORITY);
//             return;
//         }
//     }
//    }

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