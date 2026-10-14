/*
 * benchcli TEST SECS: run one of hd2bench's native CPU tests from a shell
 * (calibrating the emulator's accurate mode against a real HD2).
 * TEST: crc32 intmix dgemm saxpy memcpy latency
 */
#include "bench.c"

int main(int argc, char **argv)
{
    double secs = argc > 2 ? atof(argv[2]) : 3, r;
    const char *t = argc > 1 ? argv[1] : "";

    if (!strcmp(t, "crc32")) {
        r = JFN(cpuCrc32)(NULL, NULL, secs);
    } else if (!strcmp(t, "intmix")) {
        r = JFN(cpuIntMix)(NULL, NULL, secs);
    } else if (!strcmp(t, "dgemm")) {
        r = JFN(cpuDgemm)(NULL, NULL, secs);
    } else if (!strcmp(t, "saxpy")) {
        r = JFN(cpuSaxpy)(NULL, NULL, secs);
    } else if (!strcmp(t, "memcpy")) {
        r = JFN(memCopy)(NULL, NULL, secs);
    } else if (!strcmp(t, "latency")) {
        r = JFN(memLatency)(NULL, NULL, secs);
    } else {
        fprintf(stderr, "usage: benchcli crc32|intmix|dgemm|saxpy|memcpy|latency [secs]\n");
        return 1;
    }
    printf("%s %.3f\n", t, r);
    return 0;
}
