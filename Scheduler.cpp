//
//  Scheduler.cpp
//  CloudSim
//
//  Created by ELMOOTAZBELLAH ELNOZAHY on 10/20/24.
//

#include "Scheduler.hpp"
#include <algorithm>

/* Using this to define a template that defines upper limit of resources
   that this given task will set up.
*/
struct task_template {
    TaskClass_t t_class;
    unsigned memory_to_allocate;
    bool compute_task; // 1 for compute intensive, 0 for IO intensive
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

/* continually sort these by available memory left 
   from most memory to least memory available */
vector<VMResourcePair> IO_Linux;
vector<VMResourcePair> IO_Linuxrt;
vector<VMResourcePair> IO_Win;
vector<VMResourcePair> IO_Aix;


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

    unsigned maxNumCPUS = 0;
    unsigned maxMemory = 0;

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
        if (ratio_cpu > ratio_memory && ratio_cpu > 0.6) {
            // CPU resources dominate, so we deem this as a
            // compute intensive machine
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

}

void Scheduler::MigrationComplete(Time_t time, VMId_t vm_id) {
    // Update your data structure. The VM now can receive new tasks
}

void Scheduler::NewTask(Time_t now, TaskId_t task_id) {
    // Get the task parameters
    //  IsGPUCapable(task_id);
    //  GetMemory(task_id);
    //  RequiredVMType(task_id);
    //  RequiredSLA(task_id);
    //  RequiredCPUType(task_id);
    // Decide to attach the task to an existing VM, 
    //      vm.AddTask(taskid, Priority_T priority); or
    // Create a new VM, attach the VM to a machine
    //      VM vm(type of the VM)
    //      vm.Attach(machine_id);
    //      vm.AddTask(taskid, Priority_t priority) or
    // Turn on a machine, create a new VM, attach it to the VM, then add the task
    //
    // Turn on a machine, migrate an existing VM from a loaded machine....
    //
    // Other possibilities as desired

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