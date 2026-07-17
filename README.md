# The PHAST Memory Dependence predictor
This Gem5 v25 fork implements the PHAST MDP (https://ieeexplore.ieee.org/document/10476400), the state of the art in memory dependence prediction. This Gem5 was originally based on an optimised fork from the University of Murcia (https://github.com/CAPS-UMU/gem5), and has since been extended to include further optimisations over upstream too. 

## MDP Changes:
PHAST is much more complex than Store Sets and so required several changes and optimisations outside of just the MDP unit to work properly:
- Moved memory order violation training from IEW to commit, to avoid training on misspeculated paths.
- Prevent stores from triggering a memory order violation with loads that have either already forwarded from a younger store, or already violated with a younger store.
- Keep a record of recently executed branches from both decode and commit to index and train the predictor with respectively.
- Changed the MDP and depPred interfaces to include additional methods and arguments needed by PHAST, while also keeping it generic to allow continued use of Store Sets if desired.
- Added a memDepInfo struct to DynInst objects to track necessary information as loads are executed. 
- Added an additional commit/ROB status to specify if squashing due to memory order violation.

## UMU Optimisations:
- Fixed TAGE_SC_L_64K (based on https://github.com/useredsa/spec_tage_scl) now it is called TAGE_EMILIO
- Added (and fixed) the AssociativeBTB (based on https://github.com/dhschall/gem5-fdp)
- Ported ITTAGE indirect target predictor (from https://github.com/OpenXiangShan/GEM5)
- Several correctness and performance fixes

## Additional Optimisations:
- Includes an L3 Cache, taken from https://github.com/SamAinsworth/gem5-triangel/. Enable all of L3/2/1 at once with `--last-level-cache`. Parameters of each level is configured as normal with --lN_size, --lN_assoc, and now also --lN_mshrs.
- Latency of L2 reduced to 14 cycles from 40, as there's now an L3.
- Latency of L1-I reduced to 1 cycle to represent using a u-op cache, as per https://dl.acm.org/doi/10.1145/3613424.3614258
- Store-to-load forwarding latency is parameterised in BaseO3CPU.py as LSQForwardingLatency with a default of 4 cycles.
- Stride Prefetcher is set to a default degree of 8, prefetch_on_pf_hit is set to True and prefetch_on_access is set to False.
- Prefetchers that require access to the core's MMU don't work by default with checkpoint restoration. Modified Simulation.py to call `.registerMMU` on each prefetcher, and added `--use-fdp` to `se.py` to make fetch directed prefetching work too. This overrides `--l1i-hwp-type`.

## Using and Configuring PHAST
There isn't currently a nice python interface to select the MDP algorithm like with choosing the branch predictor. For now, just change the include file and type of the `depPred` class in `mem_dep_unit.hh` like so:
```
 //#include "cpu/o3/store_set.hh"
 #include "cpu/o3/phast.hh"
 ...
 //StoreSet depPred;
 PHAST depPred;   
 ```

PHAST's parameters can be configured in `BaseCPUO3.py` though. They are:
- phast_num_rows: Number of rows per table
- phast_associativity: Number of entries per row
- phast_tag_bits: Number of bits used for entry tags
- phast_max_counter: Max entry confidence counter value

## Reporting bugs
This implementation is new and likely still has some bugs. While it has been tested against Spec2017, your workloads may have higher violations with PHAST than with Store Sets. If this is the case, please share the details in an issue! First though, ensure the problem does not persist when `phast_max_counter` is set to a very high value, like 100. Some workloads will have more violations than Store Sets simply because we get unlucky with the frequency of specific memory dependencies, but this isn't a bug. Another issue may be due to the `depCheckShift` parameter also in `BaseCPUO3.py`. When this is >0, it introduces false dependencies which can break an assumption PHAST makes that loads are usually only dependent on one or at most two stores. Ensure PHAST does not beat Store Sets when this is also set to 0.
Also note that tests have only been run with syscall emulation mode, and full system emulation may uncover new problems too.

# The gem5 Simulator

This is the repository for the gem5 simulator. It contains the full source code
for the simulator and all tests and regressions.

The gem5 simulator is a modular platform for computer-system architecture
research, encompassing system-level architecture as well as processor
microarchitecture. It is primarily used to evaluate new hardware designs,
system software changes, and compile-time and run-time system optimizations.

The main website can be found at <http://www.gem5.org>.

## Testing status

**Note**: These regard tests run on the develop branch of gem5:
<https://github.com/gem5/gem5/tree/develop>.

[![Daily Tests](https://github.com/gem5/gem5/actions/workflows/daily-tests.yaml/badge.svg)](https://github.com/gem5/gem5/actions/workflows/daily-tests.yaml)
[![Weekly Tests](https://github.com/gem5/gem5/actions/workflows/weekly-tests.yaml/badge.svg)](https://github.com/gem5/gem5/actions/workflows/weekly-tests.yaml)
[![Compiler Tests](https://github.com/gem5/gem5/actions/workflows/compiler-tests.yaml/badge.svg)](https://github.com/gem5/gem5/actions/workflows/compiler-tests.yaml)

## Getting started

A good starting point is <http://www.gem5.org/about>, and for
more information about building the simulator and getting started
please see <http://www.gem5.org/documentation> and
<http://www.gem5.org/documentation/learning_gem5/introduction>.

## Building gem5

To build gem5, you will need the following software: g++ or clang,
Python (gem5 links in the Python interpreter), SCons, zlib, m4, and lastly
protobuf if you want trace capture and playback support. Please see
<http://www.gem5.org/documentation/general_docs/building> for more details
concerning the minimum versions of these tools.

Once you have all dependencies resolved, execute
`scons build/ALL/gem5.opt` to build an optimized version of the gem5 binary
(`gem5.opt`) containing all gem5 ISAs. If you only wish to compile gem5 to
include a single ISA, you can replace `ALL` with the name of the ISA. Valid
options include `ARM`, `NULL`, `MIPS`, `POWER`, `RISCV`, `SPARC`, and `X86`
The complete list of options can be found in the build_opts directory.

See https://www.gem5.org/documentation/general_docs/building for more
information on building gem5.

## The Source Tree

The main source tree includes these subdirectories:

* build_opts: pre-made default configurations for gem5
* build_tools: tools used internally by gem5's build process.
* configs: example simulation configuration scripts
* ext: less-common external packages needed to build gem5
* include: include files for use in other programs
* site_scons: modular components of the build system
* src: source code of the gem5 simulator. The C++ source, Python wrappers, and Python standard library are found in this directory.
* system: source for some optional system software for simulated systems
* tests: regression tests
* util: useful utility programs and files

## gem5 Resources

To run full-system simulations, you may need compiled system firmware, kernel
binaries and one or more disk images, depending on gem5's configuration and
what type of workload you're trying to run. Many of these resources can be
obtained from <https://resources.gem5.org>.

More information on gem5 Resources can be found at
<https://www.gem5.org/documentation/general_docs/gem5_resources/>.

## Getting Help, Reporting bugs, and Requesting Features

We provide a variety of channels for users and developers to get help, report
bugs, requests features, or engage in community discussions. Below
are a few of the most common we recommend using.

* **GitHub Discussions**: A GitHub Discussions page. This can be used to start
discussions or ask questions. Available at
<https://github.com/orgs/gem5/discussions>.
* **GitHub Issues**: A GitHub Issues page for reporting bugs or requesting
features. Available at <https://github.com/gem5/gem5/issues>.
* **Jira Issue Tracker**: A Jira Issue Tracker for reporting bugs or requesting
features. Available at <https://gem5.atlassian.net/>.
* **Slack**: A Slack server with a variety of channels for the gem5 community
to engage in a variety of discussions. Please visit
<https://www.gem5.org/join-slack> to join.
* **gem5-users@gem5.org**: A mailing list for users of gem5 to ask questions
or start discussions. To join the mailing list please visit
<https://www.gem5.org/mailing_lists>.
* **gem5-dev@gem5.org**: A mailing list for developers of gem5 to ask questions
or start discussions. To join the mailing list please visit
<https://www.gem5.org/mailing_lists>.

## Contributing to gem5

We hope you enjoy using gem5. When appropriate we advise charing your
contributions to the project. <https://www.gem5.org/contributing> can help you
get started. Additional information can be found in the CONTRIBUTING.md file.
