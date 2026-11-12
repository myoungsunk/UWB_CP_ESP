# Vendored project library

The src files are copied without modification from the user's provided archive:
`DIAG_R221_20260921_160325_319c65b0.zip/source/actual_library`.

The source headers retain their authorship and MIT / Apache-2.0 notices, including Thomas Trojer, Michele Biondi and Andrea Salvatori. Some files have additional notices retained in full. This package adds only Arduino library.properties and this NOTICE file; it does not claim the supplied fork is identical to a particular public upstream release.

Primary lineage: https://github.com/F-Army/arduino-dw1000-ng

The RF timing application does not call the supplied getFirstPathPower() debug-printing function. Raw power/FP/RXPACC/CI diagnostics are read without interleaving text into the binary serial stream. The legacy extra two FCS backing bytes are supplied on TX by the application.
