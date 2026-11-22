/* coop-fw: the package tag, one place (p6k). The stats file ("coop_stats 3 build <tag>"), the run file and the SysEx
 * HELLO reply all print this, so a host identifies the build over USB the same way the owner reads it off the
 * stats file. Bump it with every package (docs/device-tests.md names the packages by it). */
#ifndef COOP_BUILD_H
#define COOP_BUILD_H
#define COOP_BUILD_TAG "e16"
/* e15 (efficiency audit finding 14): the diagnostic code (the p6f / p6h / e13 cache probes and their 28.7 KB of NOP
 * functions and 40 KB of probe data, the SysEx self-test, the stage's debug compare against the stock path) is built
 * with COOP_DIAG = 1, the default: the dev package and every op1emu gate image. The release package's ELF is built
 * with -DCOOP_DIAG=0 (core/sdram/Makefile `combined-release`): the words those paths read stay (an image or policy
 * that sets them changes nothing), their code does not. */
#ifndef COOP_DIAG
#define COOP_DIAG 1
#endif
#endif
