Interesting workload:

The following is the output of my histogram after running the startup process of the LLM inference server I use for my research [link](https://github.com/ng4567/HarvestMoE). I waited till the part where the weights were being copied into the GPU's memory. I waited till the weights were transferring for about a minute (it usually takes about 20 minutes to load the model) so that I could make sure the bulk of the data was attributable to my workload. The only other workloads running on the system were the ssh daemon, standard linux proccesses and the cpu-analyzer eBPF program. This is also the all proces histogram, not the individual process histogram, and all work was done on an Azure NC80adis H100 v5 VM, with 80 CPU cores and 2 H100 GPUs.

```bash
Off-cpu time histogram
     usecs               : count    distribution
          0 -> 1          : 6097     |                                        |
          2 -> 3          : 7722     |                                        |
          4 -> 7          : 2241195  |****************************************|
          8 -> 15         : 1942252  |***********************************     |
         16 -> 31         : 26094    |                                        |
         32 -> 63         : 5892     |                                        |
         64 -> 127        : 3443     |                                        |
        128 -> 255        : 1155     |                                        |
        256 -> 511        : 1639     |                                        |
        512 -> 1023       : 2084     |                                        |
       1024 -> 2047       : 2154     |                                        |
       2048 -> 4095       : 13858    |                                        |
       4096 -> 8191       : 2617     |                                        |
       8192 -> 16383      : 1075     |                                        |
      16384 -> 32767      : 1140     |                                        |
      32768 -> 65535      : 3181     |                                        |
      65536 -> 131071     : 4997     |                                        |
     131072 -> 262143     : 10496    |                                        |
     262144 -> 524287     : 5660     |                                        |
     524288 -> 1048575    : 2270     |                                        |
    1048576 -> 2097151    : 1825     |                                        |
    2097152 -> 4194303    : 1201     |                                        |
    4194303 -> infinity   : 121      |                                        |
Blocked time histogram
     usecs                : count    distribution
          0 -> 1          : 0        |                                        |
          2 -> 3          : 87       |                                        |
          4 -> 7          : 2019657  |****************************************|
          8 -> 15         : 68916    |*                                       |
         16 -> 31         : 10696    |                                        |
         32 -> 63         : 2467     |                                        |
         64 -> 127        : 1407     |                                        |
        128 -> 255        : 1092     |                                        |
        256 -> 511        : 1637     |                                        |
        512 -> 1023       : 2065     |                                        |
       1024 -> 2047       : 2103     |                                        |
       2048 -> 4095       : 13846    |                                        |
       4096 -> 8191       : 2602     |                                        |
       8192 -> 16383      : 1075     |                                        |
      16384 -> 32767      : 1138     |                                        |
      32768 -> 65535      : 3181     |                                        |
      65536 -> 131071     : 4997     |                                        |
     131072 -> 262143     : 10496    |                                        |
     262144 -> 524287     : 5660     |                                        |
     524288 -> 1048575    : 2270     |                                        |
    1048576 -> 2097151    : 1824     |                                        |
    2097152 -> 4194303    : 1201     |                                        |
    4194303 -> infinity   : 120      |                                        |
```

I chose this workload because I thought it would involve a lot of IO and a lot of concurrent copying from DRAM to the GPU over PCIE lanes. The histograms make sense since LLM weights loading is an IO bound workload bounded by the speed of the PCIE interface. We see that the histograms are nearly identical. Threads are likely off CPU because they are blocked waiting for IO, not because they're getting prempted or voluntarily yielding. The full output (every 5 seconds can be gound in logs-moe.txt).

Citation:

The uthash C library is not mine and downloaded from: https://github.com/troydhanson/uthash/blob/master/src/uthash.h
