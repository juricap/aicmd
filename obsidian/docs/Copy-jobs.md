# Copy jobs: current increment and next design

2026-10-01. Current implementation is a simple background FIFO with progress and cancellation. Deferred conflict resolution, persistence and notifications below are the next design, not shipped features.

## Available now

- F5 copies marks, or the cursor entry if there are no marks, into the other panel's directory. The initial destination confirmation remains. F5 can enqueue more jobs while an earlier job runs.
- Local/local, local/SFTP, SFTP/local and SFTP/SFTP use the existing streaming provider and exclusive temporary-file publication. Remote/remote bytes travel through this client; server-side copy is not implemented.
- Source selection and destination are captured at submission. Browsing, switching tabs and changing marks later do not redirect queued work.
- One dedicated copy worker consumes FIFO jobs; two separate workers retain scan/view duties. A large selection is one job with sequential entries, not one thread or worker task per file.
- Admission limits: at most 64 waiting copy jobs and approximately 32 MiB of retained copy source descriptors across queued/running/uncollected work. Oversized requests return a resource-limit error without submitting a partial selection. Existing per-directory enumeration/depth/entry limits still apply.
- Status line shows job ID, queued/running/cancelling phase, cumulative bytes, completed/total **selected top-level entries**, current top-level name and pending job count. A selected directory may contain many files: this is deliberately not a fabricated recursive percentage or ETA. Transferred bytes can include a partial file that is later discarded.
- Escape cancels the displayed transfer; Shift+Escape cancels all transfers tracked by this window. Escape in the path editor still cancels that edit first. Typed `job_cancel` can target any job ID.
- Cancellation first becomes `Cancelling`, then `Cancelled` after worker cleanup. Waiting jobs are removed without waiting behind a long active copy. A blocked SSH request is interrupted through the existing child-process cancellation monitor. Local kernel I/O/flush may delay acknowledgement; the UI does not wait for it.
- Completed files remain after cancellation/failure. Partial local files are cleaned up; disconnects may leave remote `.aicmd-part-*` files. No existing destination is overwritten or merged. The first error stops that job; later queued jobs continue. The status retains the latest failure after other jobs finish. Destination panels refresh only if they still show that destination and are not already navigating.
- Jobs belong to the commander process. Closing it cancels pending/running work. No resume journal, unattended notifications or conflict inbox exists yet. Up to 256 terminal job records are retained in memory; active jobs are not pruned.

The shared core exposes job state, cancellation, byte counters and entry progress without GUI or model SDK dependencies. Workers publish progress/completions; the owning thread changes job state. This is the base for CLI/TUI/model adapters.

## Next: continue useful work while decisions wait

The worker must never open a dialog or synchronously ask a model/user to decide. Separate three records:

1. **Job**: immutable source/destination intent, policy snapshot, counters, lifecycle, owner and notification preferences.
2. **Item**: source identity/version, destination, parent/dependencies, attempts, progress, staged path and publication result.
3. **Decision**: item ID, conflict category, observed destination identity/version, allowed choices, revision token, deadline/notification status. It contains no thread, held transport lock or open destination handle.

Discover items incrementally into a bounded ready queue; large manifests spill to a transactional journal rather than growing an in-memory vector indefinitely. Use directory dependencies so unrelated siblings remain eligible. Directory merges and file/type conflicts need distinct policies; rejecting a directory must not silently discard unrelated source roots.

Suggested item states: Discovered -> Ready -> Copying -> Publishing -> Completed; conflicts -> NeedsDecision; transient errors -> RetryScheduled; explicit cancellation -> Cancelling -> Cancelled. A job can have runnable, copying and decision-waiting items simultaneously. Its aggregate state becomes WaitingForDecisions only when nothing else can run, and CompleteWithIssues if explicit skips/failures remain. Never report 100% success merely because the ready queue is empty.

On conflict: record a decision, release source/destination handles and lane locks, move to the next eligible item. If only blocked items remain, run the next job. Limit per-host/global concurrency and use fair scheduling so one huge batch or slow host cannot monopolize all I/O. Keep navigation on independent workers/lanes.

The user sees a compact nonmodal job/decision view, with keyboard access and count badges. Choices include Skip, Rename, Replace and Retry where supported; directory Merge is separately defined. Apply-to-all rules are scoped by job and conflict category, not a global silent override. Cancel remains available while decisions wait. No timeout may default to overwrite.

Resolution is a typed command such as `copy_resolve(job_id, item_id, decision_revision, choice, new_name?)`. On receiving it, revalidate source and destination identities and parent containment. A changed destination creates a fresh decision rather than applying an old approval. Rename uses validated literal path components; publication is atomic/no-replace until explicit replacement is authorized. Define platform/provider atomic-replace guarantees and fallback limitations before enabling Replace. Never delete the original first just to make room for a staged file.

Model-facing commands: `copy_enqueue`, `copy_jobs_list`, `copy_job_get`, `copy_cancel`, `copy_pause`, `copy_resume`, `copy_decisions_list`, `copy_resolve`, and `copy_notification_configure`. Return request/job/item IDs and monotonically increasing revisions; publish bounded/coalesced progress events with resumable cursors. Preserve terminal outcomes and decisions even when progress events are dropped. Models use the same policy/decision API as the GUI.

## Persistence and walking away

Before promising hours-long unattended jobs, journal queue intent, per-item publication and pending decisions. Recover idempotently after crashes: distinguish verified published files from partial staging, revalidate source versions, and reconcile interrupted publication. Do not resume an SSH transfer solely because size/mtime happen to match if stronger identity is required. Credentials stay with OpenSSH/credential stores, outside the journal.

Keep the worker host separable from the GUI so closing the window can later detach instead of cancel. The GUI should explicitly offer the implemented lifecycle; do not claim background survival while work remains inside its process.

Notifications are optional and rate-limited: completed, completed-with-issues, attention needed (no runnable work), and prolonged disconnect. First implement Windows desktop notifications plus an in-app event history. Notify once per state transition, aggregate repeated conflicts, and respect quiet hours. If the user walks away while other files can still copy, a decision notice must not pause them. External phone/email/webhook channels require explicit user configuration/authorization; do not guess addresses or send notifications now. Clicking a notification opens the corresponding job/decision, without credentials in its payload.

## Delivery and acceptance

1. **Now**: bounded FIFO, streaming progress, cancellation, continued browsing, immutable submission, no-overwrite and queue continuation after failure.
2. **Decision increment**: durable item journal, nonmodal decision inbox, skip/rename first, continue unaffected work and subsequent jobs; test stale resolutions and directory dependencies.
3. **Replacement/resume**: explicit replace/merge policies, atomic publication contracts, source-change detection and crash recovery.
4. **Unattended host**: detach/reconnect, desktop notifications, opt-in external channels, bandwidth controls and per-host fairness.

Tests include thousands of tiny files, deep/wide directories, large files, Unicode paths, all four local/SSH directions, queued and in-flight cancellation, interrupted SSH, disk-full, stale destinations, changed source, conflicts at the beginning/middle/end, model/user racing resolutions, and process restart during publication. Measure memory and navigation latency, not just throughput. Determinate totals/ETA require incremental discovery accounting; expose discovery-in-progress explicitly.

Current validation entry points: `build.bat`, `aicmd-copy-queue-tests`, existing live `aicmd-ssh-tests`, `scripts/test-ssh-gui.ps1`, and Linux builds over `ssh ai` only.
