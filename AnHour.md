machine class:
{
        Number of machines: 16
        CPU type: X86
        Number of cores: 8
        Memory: 16384
        S-States: [120, 100, 100, 80, 40, 10, 0]
        P-States: [12, 8, 6, 4]
        C-States: [12, 3, 1, 0]
        MIPS: [3000, 2400, 2000, 1500]
        GPUs: no
}

machine class:
{
        Number of machines: 16
        CPU type: X86
        Number of cores: 8
        Memory: 16384
        S-States: [120, 100, 100, 80, 40, 10, 0]
        P-States: [12, 8, 6, 4]
        C-States: [12, 3, 1, 0]
        MIPS: [3000, 2400, 2000, 1500]
        GPUs: yes
}

machine class:
{
        Number of machines: 8
        CPU type: X86
        Number of cores: 4
        Memory: 8192
        S-States: [40, 20, 16, 12, 10, 4, 0]
        P-States: [4, 2, 2, 1]
        C-States: [4, 1, 1, 0]
        MIPS: [1500, 1200, 1000, 600]
        GPUs: no
}

task class:
{
        Start time: 60000
        End time : 3600000000
        Inter arrival: 180000
        Expected runtime: 1000000
        Memory: 8
        VM type: LINUX
        GPU enabled: no
        SLA type: SLA2
        CPU type: X86
        Task type: WEB
        Seed: 520230
}

task class:
{
        Start time: 5000000
        End time :  7000000
        Inter arrival: 6000
        Expected runtime: 6000000
        Memory: 8
        VM type: LINUX
        GPU enabled: no
        SLA type: SLA0
        CPU type: X86
        Task type: WEB
        Seed: 520120
}

task class:
{
        Start time: 25000000
        End time :  27000000
        Inter arrival: 6000
        Expected runtime: 6000000
        Memory: 8
        VM type: LINUX
        GPU enabled: no
        SLA type: SLA0
        CPU type: X86
        Task type: WEB
        Seed: 520120
}

task class:
{
        Start time: 17000000
        End time :  250000000
        Inter arrival: 1000
        Expected runtime: 8000000
        Memory: 8
        VM type: LINUX
        GPU enabled: yes
        SLA type: SLA1
        CPU type: X86
        Task type: AI
        Seed: 520120
}


