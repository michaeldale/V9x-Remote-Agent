# Remote-help contribution credits

Status: proposal; not implemented  
Date: 2026-11-15  
Primary implementation: BluetraitBlog `v9x_update_checker` plugin  
Supporting implementation: this repository's CLI/MCP telemetry, if needed

## Purpose

Recognise people who lend their machines for remote development and testing.
Show what their hardware helped achieve, alongside a simple virtual points
total. Reward useful contributions without encouraging owners to leave old
hardware on indefinitely or operators to run unnecessary commands.

Use **contribution credits** as the working name. Initially these are
non-transferable recognition points with no cash value, purchases or redemption.
Keep lifetime earned credits separate from any future spendable balance; a
spending system would need its own design. No decay, streak penalties or loss of
earned credits when someone pauses remote help or takes a machine offline.

## What counts

Keep raw statistics and awarded credits separate. A profile should show:

- Machines contributed and distinct verified GPU models tested.
- Verified physical GPU cards supplied, where distinguishable; do not present
  a count of PCI models as a count of physical cards.
- Observed remote-help availability and time reserved for actual testing.
- Remote commands attempted, completed and qualifying for activity credits.
- Completed test runs, useful reproductions, regression checks and outcomes.
- Credits by category, with an explanation and link to the relevant test task.

“Computer on time” means **server-observed time available for remote help** in
v1. The server cannot prove total powered-on time from a guest uptime counter.
Show this distinction in the UI. Local/LAN-only work is outside automatic
scoring in v1 because the relay has no independent record of it.

## Proposed starting weights

These are pilot values, stored as versioned configuration rather than constants
scattered through the plugin. All awards go to the machine's verified owner.

| Contribution | Initial award | Eligibility and limits |
|---|---:|---|
| First useful test on an owner's distinct GPU model | 100 | Once per owner and normalised GPU model; registration alone earns nothing |
| First useful coverage of a GPU model across the project | +150 | Once project-wide, after review; a race-safe uniqueness constraint decides the first accepted award |
| Useful new configuration of an already-covered GPU | 25 | Reviewed, once per accepted coverage key such as GPU + OS + driver branch; no automatic award per driver build |
| Observed remote-help availability | 1 per hour | Maximum 4/day and 40/month per owner across all machines |
| Reserved testing time | 4 per hour | Maximum 16/day and 120/month per owner; replaces availability credit for the same minutes |
| Qualifying remote command activity | 1 per 10 commands | Maximum 5/day and 40/month per owner; retain remainder within the month |
| Completed test with a usable result | 10 | Once per assigned test case + relevant build/configuration; maximum 50/day from routine results |
| Accepted useful bug reproduction or diagnostic evidence | 40 | Reviewed; one outcome award per issue/contribution |
| Accepted regression/fix verification | 60 | Reviewed; one outcome award per issue/contribution |
| Significant debugging contribution | 100–300 | Explicit reviewer rationale; replaces the smaller outcome award for the same contribution |

Routine test completion and one reviewed outcome award may stack because they
recognise different work. Reproduction, verification and significant-contribution
awards do not all stack for the same contribution: upgrade by awarding only the
difference. Separate, genuinely useful contributions to the same issue can be
reviewed individually. Hardware coverage awards may stack with test awards.

This makes a newly tested GPU valuable, but not automatically more valuable than
debugging an app. Hardware expands the project's coverage; a difficult app
reproduction can unlock a fix for many users and earn just as much or more.
Do not add a speculative rarity multiplier initially. Use published, reviewed
test requests or bounties later if particular hardware is genuinely needed.

### Examples

- An owner lends a GPU model never previously tested: 100 personal coverage +
  150 project coverage + 10 completed test + 8 for two reserved test hours +
  2 for 20 qualifying commands = **270 credits**.
- An existing GPU helps solve an app problem: 200 reviewed debugging award +
  10 completed test + 8 for two reserved test hours + 2 for 20 qualifying
  commands = **220 credits**. A major 300-credit outcome would total **320**.
- Leaving a machine available for 24 hours without testing earns at most
  **4 credits that day**, and availability stops earning at 40 that month.
- Reinstalling the agent or repeating the same commands does not create fresh
  hardware coverage or new test outcomes.

Examples assume no prior usage of the applicable daily/monthly caps.

## Measurement and evidence

### Availability and reserved time

Use server receipt timestamps from accepted polls with remote help On. Count
only the interval between consecutive eligible polls when the gap is no more
than 120 seconds. Longer gaps earn no time; Paused/Off and stale machines earn
none. Use elapsed time, not poll count, so the 2-second active poll interval
does not earn more than the normal 60-second interval.

A test reservation must link an operator, machine and test task, with an explicit
start/end and a renewable lease (initially 15 minutes). A registered long-running
test may extend that lease to its bounded deadline; an abandoned open session
alone is insufficient. Initially require eligible availability polls for both
time categories. Reboot/offline gaps are not credited automatically; an operator
may record a reviewed adjustment for useful testing disrupted by a crash.

Union overlapping intervals across an owner's machines before calculating time.
Reserved minutes take precedence over availability minutes. Accumulate seconds
and use integer sub-credit units internally; round only for display. Apply daily
and monthly limits in UTC and label the accounting period clearly in the UI.

### Remote commands

The current relay forwards opaque signed byte chunks. A chunk, HTTP request or
session is **not a command**. Do not infer command counts from transport traffic
or teach the relay to interpret every guest protocol verb just for scoring.

Add an authenticated operator-side completion event from the shared CLI/MCP
execution path, with a stable command execution ID, machine/session ID, task ID,
verb category, timestamps and outcome. Preserve that ID across transport retries
and queued event delivery. Record failures separately; a crash can be valuable
evidence and qualify for a reviewed outcome even if the command did not succeed.

Count logical test/work operations, not keepalives, discovery, info/status polls,
wait polling iterations, retries, chat, notes or telemetry itself. Record these
excluded operations as raw activity if useful. A long benchmark is one command
and can earn testing-time/result credits without repeated polling. Operators
must explicitly associate qualifying operations with a test task; repeated
equivalent work on the same task/configuration needs a new test-run ID and reason.

Operator telemetry is trusted accounting evidence, not cryptographic proof that
a physical machine performed the work. Until that instrumentation exists, show
session statistics and award reviewed outcomes; do not invent command counts.

### Hardware and ownership

Capture an inventory snapshot for accepted test runs. Normalise GPU vendor/device
IDs and reviewed model aliases; retain subsystem IDs, revision, OS and driver
information for configuration coverage. Distinguish physical and virtual adapters.
VMs can earn useful testing credits, but cannot claim new physical GPU coverage.

Neither PCI IDs nor install IDs identify a unique physical card reliably. Use a
reviewed hardware asset record to track card swaps and duplicates; absent adequate
evidence, report distinct models only. Multiple cards of the same model earn no
automatic extra hardware award. Reinstall, rename, ownership transfer and machine
re-registration preserve the asset's project coverage history.

Link machines to an authenticated BluetraitBlog account through an owner-approved
claim flow. A supplied GitHub handle or email is not proof of ownership. Unclaimed
machines can accumulate pending evidence, but do not receive account credits until
the claim is verified. Ownership changes affect future contributions; historical
awards remain with the contributor at the time of the work.

## Plugin design

Keep scoring server-side in `v9x_update_checker`, with an isolated contribution
service so it could later become a separate plugin without changing the agent.
Inspect the actual plugin schema and account APIs before choosing migration names.
The following are proposed logical records, not existing tables or endpoints:

- **Owner/machine claims:** account, machine/install identity, effective dates,
  verification and audited transfers.
- **Hardware assets and coverage:** normalised model/configuration, evidence,
  physical/virtual classification, first accepted test and unique award keys.
- **Test tasks/runs and reservations:** operator, machine, configuration/build,
  start/end/lease, result, issue reference and reviewer.
- **Contribution events:** immutable source ID, event type, server receipt time,
  occurrence time, owner-at-event, machine/session/task, bounded metadata and
  evidence reference. Deduplicate using a unique source/event ID.
- **Credit ledger:** owner, event/award key, category, signed amount, rule version,
  accounting period, approval state and reversal/adjustment reference. Derive
  totals from the ledger; cached totals must be rebuildable.
- **Rule versions:** weights, caps, effective date and change rationale.

Persist events and ledger awards transactionally with unique constraints so
parallel sessions, retries and replayed jobs cannot double-award. Serialize cap
allocation per owner/accounting period. Close time buckets deterministically;
late evidence produces an explicit adjustment, not a silently changed total.
Freeze the relevant scoring rule version at contribution time and retain it on
the award. Routine weight changes apply prospectively; corrections use reversible
ledger entries with a reason and audit trail, never deletion of history.

Proposed operations: ingest operator completion/test events; claim a machine;
read owner totals/history; review outcomes; approve/reverse an award; configure
future rule versions. Separate owner, operator and reviewer permissions. For the
pilot, require a different reviewer for discretionary awards to an operator's
own account. Keep the existing signing key on the operator's PC: credits must
not change the remote-control trust model or block testing if scoring is down.

## Owner and operator experience

Show the owner a private breakdown: “Your Radeon test earned 100 hardware credits
and 10 test credits”, pending/reviewed status, caps reached and recent outcomes.
Show both lifetime and monthly contribution totals, with hardware/test badges.
Offer an optional public alias and opt-in leaderboard; keep email, IP, computer
name, secrets and raw command arguments private. Public issue/test links must
also be explicitly publishable. Explain how to request a correction.

On the operator dashboard, attach a test task before working, display the current
reservation and offer a short end-of-run result form. Do not make an owner approve
each command to receive credit. Pausing or withdrawing consent remains immediate
and never requires forfeiting existing credits.

Store minimal event metadata rather than command output or transferred file
contents. Proposed default: raw scoring evidence retained for 90 days, with
long-lived ledger summaries and approved outcome references; align deletion and
account removal with the site's existing policy before implementation.

## Delivery plan

1. **Inspect and instrument:** confirm plugin schema, account/claim mechanism and
   polling/session hooks. Add task/run records and availability measurement in
   shadow mode. No agent update required for basic polling-based availability.
2. **Pilot private credits:** implement claims, versioned rules, ledger, hardware
   review and manual outcome awards. Run a four-week pilot with a small consenting
   group. Clearly label balances provisional during shadow scoring.
3. **Command and reservation integration:** add completion events to CLI/MCP,
   bounded durable retry delivery and task-aware reservations. Older clients keep
   working but do not receive inferred command awards. Scoring outages must not
   interrupt remote testing.
4. **Review weights:** compare hardware, time, commands and outcome shares; inspect
   outliers and owner feedback. Tune future weights so useful results dominate
   unattended availability and command volume. Publish the rules before launch.
5. **Public recognition:** opt-in profiles, badges and monthly/lifetime views once
   identity, duplicate prevention and correction workflows have been exercised.

Do not retroactively estimate credits from machine age or current uptime. Import
historical contributions only where evidence exists, as separately labelled,
reviewed legacy awards.

## Acceptance checks before enabling real awards

- Replayed events, three concurrent relay sessions and worker retries award once.
- Concurrent owners claiming first project coverage cannot both earn that bonus.
- A reinstall, account transfer or second machine with the same GPU model cannot
  farm hardware awards; genuinely new coverage can still be reviewed.
- Poll frequency changes, pause/off transitions, stale gaps, long benchmarks,
  reboot gaps and overlapping machines produce the specified time calculation.
- Waits/keepalives earn no command credits; retries reuse their original ID.
- Daily/monthly boundaries, partial credits, caps, late events and rule changes
  have deterministic results; rebuilds match cached balances.
- Faulting tests can receive useful-result awards; abandoned tasks do not earn
  endless reserved time. Reversals and upgrades preserve an explainable ledger.
- Claim spoofing, unauthorized awards and private metadata in public profiles
  are rejected. Scoring downtime does not affect relay control or owner pause.

## Decisions to settle during the pilot

- Do the initial 100 + 150 hardware awards reflect the real value of new coverage?
- Is 100–300 enough range for difficult app/driver debugging, and who reviews it?
- Should availability earn any points, or become a statistic/badge only?
- Are per-owner caps fair for contributors with several genuinely useful machines?
- Should requested rare configurations get a published fixed bounty? If so,
  define eligibility and whether it replaces the ordinary coverage award upfront.

Related: [remote-help protocol and deployment](remote-help.md) and
[relay architecture decision](decisions/2026-10-20-remote-help-relay.md).
