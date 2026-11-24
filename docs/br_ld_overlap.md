# Branch / independent-load temporal overlap

This observer measures correct-path CF_CBR main-predictor direction errors that recover at execute. It does not measure cycles saved or reproduce Eyerman Figure 14 exactly. Default is disabled.

Enable with `--br_ld_overlap_enable 1`. `--br_ld_overlap_selftest 1` runs the observer state machine tests during simulator initialization; build/run through SCI MCP. `--br_ld_overlap_debug_events 1` writes per-branch JSONL audit records in OUTPUT_DIR for short validation runs.

The cohort is dispatched during the measured ROI, beginning at the normal FULL_WARMUP stats checkpoint. An older load already in flight at that boundary remains eligible. No WARMUP fast-only mode, PERIODIC_DUMP, or mid-run CLEAR_STATS is used by the descriptors. Observer state is per core; production descriptors use one core.

Load latency is first accepted execution issue to REG_DATA_DEP result-ready cycle, including retries and resource waiting after issue. A candidate is any older correct-path demand load not ready at branch dispatch, even if not yet issued. Dynamic register and memory source edges propagate outstanding-load ancestry. Missing live ancestry is recorded and censors the window; it never becomes independence. Retired producers cannot carry a still-incomplete older load under in-order retirement.

Windows are half-open: dispatch to first younger correct-path dispatch after the actual recovery. They split at the branch execution-completion timestamp, not the time the recovery scheduling function runs. Each branch's load intervals are unioned, so concurrent loads do not multiply cycles. The sum across branch windows is branch-attributed time, not unique execution time or eliminated stall time.

Seven strata are emitted: T50, T100, T200, LLC, LLC_T50, LLC_T100, LLC_T200. Internal memory `l1_miss` denotes LLC miss in PARAMS.golden_cove. Demand loads joining an unsatisfied missed request qualify; hardware prefetches themselves do not. Fast demand merges can qualify as LLC without meeting any latency threshold.

Each stratum has OVERLAP, AT_RESOLVE, CYCLES, PRE_CYCLES, POST_CYCLES; DEP_NONE/DEP_ONLY/INDEP_ONLY/BOTH; REC_NONE/REC_PARTIAL/REC_FULL. Count operands are raw; BRLD_NORM_* FLOAT operands are per million retired ROI instructions for weighted sum-before-ratio analysis. BRLD_ROI_INSTRUCTIONS is measured explicitly at the stats checkpoint. BRLD_BRANCHES_TOTAL = COMPLETE + CENSORED. End-of-run missing timestamps/ancestry censor the entire branch consistently across strata. Squashed candidates are excluded and counted separately.

Validation compares baseline / observer-off / observer-on deterministic collected stats, tests synthetic observer events, independently reconstructs all event-log unions, and checks all counter invariants. Use the project's analysis/analyze.py after SCI collection; it requires the CSV schema sidecar and complete SimPoint coverage.
