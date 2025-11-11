//
//  Scheduler.hpp
//  CloudSim
//
//  Created by ELMOOTAZBELLAH ELNOZAHY on 10/20/24.
//

#ifndef Scheduler_hpp
#define Scheduler_hpp

#include <vector>

#include "Interfaces.h"

class Scheduler {
public:
    Scheduler()                 {}
    void Init();
    void MigrationComplete(Time_t time, VMId_t vm_id);
    void NewTask(Time_t now, TaskId_t task_id);
    void PeriodicCheck(Time_t now);
    void Shutdown(Time_t now);
    void TaskComplete(Time_t now, TaskId_t task_id);
    void AllocateNewLinuxVM(TaskId_t task_id, TaskInfo_t t_info, bool compute_task);
    void AllocateNewWinVM(TaskId_t task_id, TaskInfo_t t_info, bool compute_task);
    void AllocateNewLinuxRTVM(TaskId_t task_id, TaskInfo_t t_info, bool compute_task);
    void AllocateNewAixVM(TaskId_t task_id, TaskInfo_t t_info, bool compute_task);
private:
    vector<VMId_t> vms;
    vector<MachineId_t> machines;
};



#endif /* Scheduler_hpp */
