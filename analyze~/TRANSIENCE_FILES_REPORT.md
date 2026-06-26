# Transience Implementation Report: Role of Source Files with DLL-Based Architecture

## Overview
This report explains the necessity of maintaining `cumulative_transience.h` and `cumulative_transience.c` within the `analyze~` repository, even when the analysis logic is primarily delivered via `libtransience.dll`.

## The Necessity of the Header File (`.h`)
The `cumulative_transience.h` file is strictly required for the compilation of the `analyze~` Max object. Even when linking dynamically to a DLL, the compiler needs to know:
1.  **Data Structure Layouts**: Structures like `TransientAnalyzer`, `FullAnalysisResult`, and `PeakResult` are shared between the DLL and the Max object. The compiler must know the exact size and alignment of these structures to generate code that correctly accesses their members.
2.  **Function Prototypes**: The `analyze~.c` code uses function pointers (via `GetProcAddress`) that are cast to specific function signatures (e.g., `analyzer_create_ptr`). These signatures are derived from the prototypes defined in the header file.

Without the header file, `analyze~.c` cannot be compiled into `analyze~.mxe64`.

## The Role of the Source File (`.c`)
While `cumulative_transience.c` is not strictly necessary for the *build* or *runtime* of the DLL-based Max object, it serves several critical purposes in this repository:
1.  **Authoritative Source of Truth**: The user noted that the DLL provides a "standardized algorithm" used by disparate codebases. Maintaining the source file alongside the binary ensures that the specific version of the algorithm contained in the DLL is documented and portable.
2.  **Portability and Re-compilation**: Should the object need to be ported to a different architecture (e.g., macOS, Linux) where a compatible DLL is not yet available, having the source file allows for immediate static linking or the creation of a new shared library.
3.  **Cross-Compilation**: For developers working in environments like the current sandbox, having the source allows for functional verification and testing of the algorithm's logic independently of the Windows-specific DLL.

## Responsiveness Improvements
By reducing the analysis window (`analysis_seconds`) within the Max object's worker task from 15 seconds to 2 seconds, the computational load per DLL call has been significantly reduced. This allows the background worker to complete its tasks within the target 100ms hop interval, resolving the issue where output was previously delayed by roughly one second.

## Conclusion
The header file is a technical requirement for compilation, while the source file is maintained to ensure the algorithm's integrity, portability, and transparency across the project.
