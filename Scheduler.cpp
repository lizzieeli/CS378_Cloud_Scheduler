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
struct MResourcePair {
    MachineId_t m_id;
    Time_t pending_execution_time;
    unsigned avail_mem;
};

vector<MachineId_t> ArmMachines;
vector<MachineId_t> PowerMachines;
vector<MachineId_t> RiscvMachines;
vector<MachineId_t> X86Machines;

vector<MachineId_t> ComputeMachines;
vector<MachineId_t> MemoryMachines;

/* all data structures to continuously maintain */
map<TaskId_t, task_template> task_temp_mappings;
map<TaskId_t, VMId_t> vm_mappings;

/* everytime we create a new vm or migrate a vm to another machine */
map<VMId_t, MachineId_t> vm_to_m_mappings;
map<MachineId_t, vector<VMId_t>> m_to_vm_mappings;
map<VMId_t, MachineId_t> vms_migrating_with_old_machine;


/* for migration bookkeeping to maintain correctness */
vector<MachineId_t> m_changing_state; // we know all machines in this can be for compute tasks
vector<MachineId_t> powered_down_machines; // we know all machines in this can be for compute tasks

/* for initialization and identifying diff types of machines */
unsigned maxNumCPUS;
unsigned maxMIPS;
unsigned maxMemory;

unsigned numARM;
unsigned numRISCV;
unsigned numPOWER;
unsigned numX86;

unsigned compute_m_to_io_m;
unsigned compute_m_wakeup;

/*  The point of this function is to calculate the pending execution time of a given VM.
    It goes through to get all the remaining total instructions left (from all its active tasks)
    then gets the MIPS based on the current p state of the machine this vm is attached to.
    Additionally, we need to get the total number of cpus that the physical machine has.
    From there, we can calculate the remaining expected time of execution from this point.
*/
static Time_t FindRemainingExecTime(MachineId_t this_m) {
    Time_t remaining_exec_time = 0;
    vector<VMId_t> this_vm_vector = m_to_vm_mappings[this_m];
    for (VMId_t vm_attached: this_vm_vector) {
        VMInfo_t vm_info = VM_GetInfo(vm_attached);
        uint64_t total_remaining_instr = 0;
        for (TaskId_t active_task: vm_info.active_tasks) {
            total_remaining_instr += GetTaskInfo(active_task).remaining_instructions;
        }
        MachineInfo_t m_info = Machine_GetInfo(this_m);
        unsigned int instructions_per_sec = m_info.performance[m_info.p_state] * 1000000;
        // get the MIPS rating so we can do remaining_instr / MIPS to get seconds remaining for a given task
        remaining_exec_time = (total_remaining_instr / (instructions_per_sec * m_info.num_cpus)) * 1000000; // conversion from seconds to microseconds
    }
    return remaining_exec_time; // in microseconds
}

/*  The point of this function is to calculate the remaining
    available memory of a machine
*/
static unsigned FindRemainingAvailMem(MachineId_t m_id) {
    MachineInfo_t m_info = Machine_GetInfo(m_id);
    return m_info.memory_size - m_info.memory_used;
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

/* Helper function to get the first available vm of the required type we can find on this
   given machine. If there are none, make and attach a new one and return it.
*/
static VMId_t find_suitable_VM_on_M(MachineId_t m_id, VMType_t req_vm) {
    for (VMId_t this_vm : m_to_vm_mappings[m_id]) {
        if (VM_GetInfo(this_vm).vm_type == req_vm) {
            return this_vm;
        }
    }
    // if we reach here, need to make new vm of required type, attach to given machine
    VMId_t vm_created = VM_Create(req_vm, Machine_GetInfo(m_id).cpu);
    VM_Attach(vm_created, m_id);
    // update necesssary data structures
    vm_to_m_mappings[vm_created] = m_id;
    m_to_vm_mappings[m_id].push_back(vm_created);
    return vm_created;
}

/* These functions will help in optimizing for energy and also performance
   when an io task completes by checking to see if any further load balancing can be done
*/
static void load_balance_IO_Machines() {
    // sort all Memory Machines based on ascending pending execution time
    vector<MResourcePair> sorted_io_machines;
    for (MachineId_t m_id : MemoryMachines) {
        Time_t pending_execution_time = FindRemainingExecTime(m_id);
        unsigned avail_mem = FindRemainingAvailMem(m_id);
        sorted_io_machines.push_back({m_id, pending_execution_time, avail_mem});
    }
    // sort by ascending pending execution time
    sort(sorted_io_machines.begin(), sorted_io_machines.end(),
        [](const MResourcePair& a, MResourcePair& b){
            return a.pending_execution_time < b.pending_execution_time;
        });

    vector<MResourcePair> OverloadedMs;
    vector<MResourcePair> UnderloadedMs;
    for (unsigned i = 0; i < sorted_io_machines.size()/2; i++) {
        UnderloadedMs.push_back(sorted_io_machines[i]);
    }
    for (unsigned i = sorted_io_machines.size()/2; i < sorted_io_machines.size(); i++) {
        OverloadedMs.push_back(sorted_io_machines[i]);
    }

    unsigned underloaded_index = 0;
    for (MResourcePair m_pair: OverloadedMs) {
        if (Machine_GetInfo(m_pair.m_id).active_tasks > 0) {
            for (VMId_t this_vm : m_to_vm_mappings[m_pair.m_id]) {
                unsigned this_vm_num_active = VM_GetInfo(this_vm).active_tasks.size();
                if (this_vm_num_active > 0) {
                    TaskId_t task_to_migrate = VM_GetInfo(this_vm).active_tasks[0];
                    TaskInfo_t t_info = GetTaskInfo(task_to_migrate);
                    for (unsigned i = underloaded_index; i < UnderloadedMs.size(); i++) {
                        if (Machine_GetInfo(UnderloadedMs[underloaded_index].m_id).cpu == Machine_GetInfo(OverloadedMs[i].m_id).cpu) {
                            Time_t difference = OverloadedMs[i].pending_execution_time - UnderloadedMs[underloaded_index].pending_execution_time;
                            if (difference <= 1000000) {
                                // at this point all the machines left to look through are almost equal to each other in load
                                return;
                            }
                            // remove task from chosen overloaded machine
                            VM_RemoveTask(this_vm, task_to_migrate);
                            // add task to chosen underloaded machine
                            VMId_t vm_to_migrate_to = find_suitable_VM_on_M(UnderloadedMs[underloaded_index].m_id, t_info.required_vm);
                            VM_AddTask(vm_to_migrate_to, task_to_migrate, HIGH_PRIORITY);
                            // update data structures and other logic necessary
                            vm_mappings[task_to_migrate] = vm_to_migrate_to;
                            underloaded_index++;
                        }
                    }
                }
            } 
        }      
    }

}

/* This is a helper function to move all the vms on m2 to m1 and power down m2
*/
static void migrate_all_vms_and_power_down(MachineId_t m1, MachineId_t m2) {
    ComputeMachines.erase(remove(ComputeMachines.begin(), ComputeMachines.end(), m1), ComputeMachines.end());
    ComputeMachines.erase(remove(ComputeMachines.begin(), ComputeMachines.end(), m2), ComputeMachines.end());
    vector<VMId_t> vms_on_m2 = m_to_vm_mappings[m2];
    for (VMId_t this_vm : vms_on_m2) {
        vms_migrating_with_old_machine[this_vm] = m2;
        m_to_vm_mappings[m1].push_back(this_vm);
        m_to_vm_mappings[m2].erase(remove(m_to_vm_mappings[m2].begin(), m_to_vm_mappings[m2].end(), this_vm), m_to_vm_mappings[m2].end());
        vm_to_m_mappings[this_vm] = m1;
        m_changing_state.push_back(m2);
        VM_Migrate(this_vm, m1);
    }
}

/* This function is for trying to consolidate as many compute intensive vm's
   onto one compute machine
*/
static void consolidate_vms() {
    // first go through and calculate the resources (memory and remaining execution time)
    // of every compute machine that is up.
    vector<MResourcePair> current_m_compute_resources;
    for (MachineId_t m_id : ComputeMachines) {
        Time_t remaining_exec_time = FindRemainingExecTime(m_id);
        unsigned avail_mem = FindRemainingAvailMem(m_id);
        current_m_compute_resources.push_back({m_id, remaining_exec_time, avail_mem});
    }
    // then try to migrate all vm's of one machine to another if possible and turn off that machine.
    for (MResourcePair m_pair1 : current_m_compute_resources) {
        MachineInfo_t m_info1 = Machine_GetInfo(m_pair1.m_id);
        for (MResourcePair m_pair2 : current_m_compute_resources) {
            MachineInfo_t m_info2 = Machine_GetInfo(m_pair2.m_id);
            if (m_pair1.m_id != m_pair2.m_id && m_info1.cpu == m_info2.cpu) {
                if (m_pair1.avail_mem >= m_info2.memory_used) {
                    migrate_all_vms_and_power_down(m_pair1.m_id, m_pair2.m_id);
                    return;
                }
                else if (m_pair2.avail_mem >= m_info1.memory_used) {
                    migrate_all_vms_and_power_down(m_pair2.m_id, m_pair1.m_id);
                    return;
                }
            }
        }
    }

}

void Scheduler::HandleStateChangeComplete(MachineId_t m_id) {
    m_changing_state.erase(remove(m_changing_state.begin(), m_changing_state.end(), m_id), m_changing_state.end());
    if (Machine_GetInfo(m_id).s_state == S5) {
        powered_down_machines.push_back(m_id);
    }
    if (Machine_GetInfo(m_id).s_state == S0 && compute_m_to_io_m > 0) {
        MemoryMachines.push_back(m_id);
        compute_m_to_io_m--;
    }
    else if (Machine_GetInfo(m_id).s_state == S0 && compute_m_wakeup > 0) {
        ComputeMachines.push_back(m_id);
        compute_m_wakeup--;
    }
}

void Scheduler::Init() {
    SimOutput("Scheduler::Init(): Total number of machines is " + to_string(Machine_GetTotal()), 3);
    SimOutput("Scheduler::Init(): Initializing scheduler", 1);

    numARM = 0;
    numRISCV = 0;
    numPOWER = 0;
    numX86 = 0;

    maxNumCPUS = 0;
    maxMemory = 0;
    maxMIPS = 0;

    compute_m_to_io_m = 0;
    compute_m_wakeup = 0;

    unsigned total_machines = Machine_GetTotal();


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
            ComputeMachines.size() > MemoryMachines.size() ? MemoryMachines.push_back(MachineId_t(i)) : ComputeMachines.push_back(MachineId_t(i));
        }
    }

    // start with initializing at least one linux vm per IO machine
    // and initialize one linux machine on only one compute machine, sleeping the rest
    MachineInfo_t m_info = Machine_GetInfo(ComputeMachines[0]);
    VMId_t new_linux_vm = VM_Create(LINUX, m_info.cpu);
    VM_Attach(new_linux_vm, ComputeMachines[0]);
    // update all necessary data structures
    m_to_vm_mappings[ComputeMachines[0]].push_back(new_linux_vm);
    vm_to_m_mappings[new_linux_vm] = ComputeMachines[0];

    // put to sleep all other compute machines
    for (unsigned i = 1; i < ComputeMachines.size(); i++) {
        Machine_SetState(ComputeMachines[i], S5);
        m_changing_state.push_back(ComputeMachines[i]);
    }
    for (unsigned i = 1; i < ComputeMachines.size(); i++) {
        ComputeMachines.erase(ComputeMachines.begin() + i);
    }
    for (unsigned i = 0; i < MemoryMachines.size(); i++) {
        MachineInfo_t m_info = Machine_GetInfo(MemoryMachines[i]);
        VMId_t new_linux_vm = VM_Create(LINUX, m_info.cpu);
        VM_Attach(new_linux_vm, MemoryMachines[i]);
        // update all necessary data structures
        m_to_vm_mappings[MemoryMachines[i]].push_back(new_linux_vm);
        vm_to_m_mappings[new_linux_vm] = MemoryMachines[i];
    }

    // debugging statements
    cout << "number of compute machines found: " << ComputeMachines.size() << endl;
    cout << "number of io machines found: " << MemoryMachines.size() << endl;

}

void Scheduler::MigrationComplete(Time_t time, VMId_t vm_id) {
    // Update your data structure
    MachineId_t old_m_id = vms_migrating_with_old_machine[vm_id];
    vms_migrating_with_old_machine.erase(vm_id);
    MachineId_t m_id = vm_to_m_mappings[vm_id];
    for (map<VMId_t, MachineId_t>::iterator it = vms_migrating_with_old_machine.begin(); it != vms_migrating_with_old_machine.end(); it++) {
        if (it->second == old_m_id) {
            return;
        }
    }
    // if we reached here we finally sucessfully migrated all vm's on old machine. can call to power it down
    m_changing_state.push_back(old_m_id);
    Machine_SetState(old_m_id, S5);
    ComputeMachines.push_back(m_id);
}

void Scheduler::NewTask(Time_t now, TaskId_t task_id) {
    task_template new_template = TemplateExtraction(task_id, now);
    task_temp_mappings[task_id] = new_template;
    TaskInfo_t t_info = GetTaskInfo(task_id);

    vector<MResourcePair> m_sorted_resource;
    vector<MResourcePair> adjusted_sorted_resource;

    // only looking through the pool of machines that match this task's required cpu type and task type
    if (new_template.compute_task) {
        for (MachineId_t m_id: ComputeMachines) {
            if (Machine_GetInfo(m_id).cpu == t_info.required_cpu) {
                Time_t pending_execution_time = FindRemainingExecTime(m_id);
                unsigned available_mem = FindRemainingAvailMem(m_id);
                if (available_mem - t_info.required_memory > 0) {
                     m_sorted_resource.push_back({m_id, pending_execution_time, available_mem});
                }
            }
        }
        // sort by descending available memory order
        sort(m_sorted_resource.begin(), m_sorted_resource.end(),
            [](const MResourcePair& a, MResourcePair& b){
                return a.avail_mem > b.avail_mem;
            });
    }
    else {
        for (MachineId_t m_id: MemoryMachines) {
            if (Machine_GetInfo(m_id).cpu == t_info.required_cpu) {
                Time_t pending_execution_time = FindRemainingExecTime(m_id);
                unsigned available_mem = FindRemainingAvailMem(m_id);
                if (available_mem - t_info.required_memory > 0) {
                    m_sorted_resource.push_back({m_id, pending_execution_time, available_mem});
                }
            }
        }
        // sort based on ascending pending execution time
        sort(m_sorted_resource.begin(), m_sorted_resource.end(),
            [](const MResourcePair& a, MResourcePair& b){
                return a.pending_execution_time > b.pending_execution_time;
            });
   }


    for (unsigned i = 0; i < m_sorted_resource.size(); i++) {
        MResourcePair m_pair = m_sorted_resource[i];
        MachineId_t possible_m = m_pair.m_id;
        MachineInfo_t m_info = Machine_GetInfo(possible_m);
        vector<VMId_t> this_vm_vector = m_to_vm_mappings[m_pair.m_id];
        for (VMId_t this_vm: this_vm_vector) {
            if (VM_GetInfo(this_vm).vm_type == t_info.required_vm) {
                // don't need to do a migration check bc pr sure this data structure will be synchronized for all migration calls
                VM_AddTask(this_vm, task_id, HIGH_PRIORITY);
                // update necessary datastructs
                vm_mappings[task_id] = this_vm;
                return;
            }
        }
    }

    if (m_sorted_resource.size() == 0) {
        // if we reached here, there are no available machines that fit this task type at all
        // need to dynamically re-classify unused machines
        if (new_template.compute_task) {    // if it is a compute task, we need to look at unused io machines
            // wake up another compute machine
            if (powered_down_machines.size() > 0) {
                MachineId_t m_id = powered_down_machines[0];
                powered_down_machines.erase(powered_down_machines.begin());
                m_changing_state.push_back(m_id);
                ComputeMachines.erase(remove(ComputeMachines.begin(), ComputeMachines.end(), m_id), ComputeMachines.end());
                Machine_SetState(m_id, S0);
                compute_m_wakeup++;
            }
            for (MachineId_t m_id: MemoryMachines) {
                MachineInfo_t m_info = Machine_GetInfo(m_id);
                if (m_info.cpu == t_info.required_cpu && m_info.active_tasks == 0) {
                    MemoryMachines.erase(remove(MemoryMachines.begin(), MemoryMachines.end(), m_id), MemoryMachines.end());
                    ComputeMachines.push_back(m_id);
                    for (VMId_t vm_id: m_to_vm_mappings[m_id]) {
                        if (VM_GetInfo(vm_id).vm_type == t_info.required_vm) {
                            VM_AddTask(vm_id, task_id, HIGH_PRIORITY);
                            vm_mappings[task_id] = vm_id;
                            return;
                        }
                    }
                    // create new vm for this machine that matches everything
                    VMId_t new_vm = VM_Create(t_info.required_vm, t_info.required_cpu);
                    VM_Attach(new_vm, m_id);
                    VM_AddTask(new_vm, task_id, HIGH_PRIORITY);
                    // update data structures
                    m_to_vm_mappings[m_id].push_back(new_vm);
                    vm_mappings[task_id] = new_vm;
                    vm_to_m_mappings[new_vm] = m_id;
                    return;
                }
            }
        }
        else {  // if it is an io task, we need to add task to a compute machine and make sure to turn a powered down machine to an IO machine
            for (MachineId_t m_id: ComputeMachines) {
                MachineInfo_t m_info = Machine_GetInfo(m_id);
                if ((m_info.cpu == t_info.required_cpu) && (m_info.memory_size - m_info.memory_used > t_info.required_memory)) {
                    for (VMId_t vm_id: m_to_vm_mappings[m_id]) {
                        if (VM_GetInfo(vm_id).vm_type == t_info.required_vm) {
                            VM_AddTask(vm_id, task_id, HIGH_PRIORITY);
                            vm_mappings[task_id] = vm_id;
                            return;
                        }
                    }
                    // create new vm for this machine that matches everything
                    VMId_t new_vm = VM_Create(t_info.required_vm, t_info.required_cpu);
                    VM_Attach(new_vm, m_id);
                    VM_AddTask(new_vm, task_id, HIGH_PRIORITY);
                    // update data structures
                    m_to_vm_mappings[m_id].push_back(new_vm);
                    vm_mappings[task_id] = new_vm;
                    vm_to_m_mappings[new_vm] = m_id;
                    return;
                }
            }
            MachineId_t m_id = powered_down_machines[0];
            powered_down_machines.erase(powered_down_machines.begin());
            m_changing_state.push_back(m_id);
            ComputeMachines.erase(remove(ComputeMachines.begin(), ComputeMachines.end(), m_id), ComputeMachines.end());
            Machine_SetState(m_id, S0);
            compute_m_to_io_m++;
            return;
        }
    }
    else {
        // there were simply just no vm's of the type we need. pick the first machine that works and add a vm to it
        // then add task to that vm
        for (unsigned i = 0; i < m_sorted_resource.size(); i++) {
            MResourcePair m_pair = m_sorted_resource[i];
            MachineId_t possible_m = m_pair.m_id;
            MachineInfo_t m_info = Machine_GetInfo(possible_m);
            VMId_t new_vm = VM_Create(t_info.required_vm, t_info.required_cpu);
            VM_Attach(new_vm, m_pair.m_id);
            VM_AddTask(new_vm, task_id, HIGH_PRIORITY);
            // update data structures
            m_to_vm_mappings[m_pair.m_id].push_back(new_vm);
            vm_mappings[task_id] = new_vm;
            vm_to_m_mappings[new_vm] = m_pair.m_id;
            return;
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
    SimOutput("Scheduler::TaskComplete(): Task " + to_string(task_id) + " is complete at " + to_string(now), 4);
    task_temp_mappings.erase(task_id);
    vm_mappings.erase(task_id);
    consolidate_vms();
    load_balance_IO_Machines();
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
    Scheduler.HandleStateChangeComplete(machine_id);
}