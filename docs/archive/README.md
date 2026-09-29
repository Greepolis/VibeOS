# Archive

Documents that described the project at a moment that has passed. Kept because
they are the record of what was believed and planned then, not because they
describe the kernel now - for that, start at
[implementation_progress.md](../implementation_progress.md) and the plan
directories (`mm/`, `sched/`, `exec/`, `io/`, `core/`, `abi/`).

| Document | What it was | Superseded by |
| --- | --- | --- |
| [project_status_assessment_2026-04-03.md](project_status_assessment_2026-04-03.md) | A status snapshot from when the kernel ran only as a host simulation | [implementation_progress.md](../implementation_progress.md) |
| [architectural_review_for_developer_2026-04-03.md](architectural_review_for_developer_2026-04-03.md) | The architecture review of the same date | the plan directories, and the reviews in `implementation_progress/` |
| [storage_plan.md](storage_plan.md) | The storage plan, written when FAT was called directly from twenty places and there was no filesystem layer | [io/](../io/README.md), which carried it out |
| [test_automation_spec.md](test_automation_spec.md), [test_feedback_profiles.md](test_feedback_profiles.md) | A test-automation design whose profiles were never built | [testing_strategy.md](../testing_strategy.md) and `scripts/dev/README.md`, which describe what runs |
| [milestones_2026-08.md](milestones_2026-08.md) | The milestone table from `roadmap/`, last snapshot 2026-08-03 | [roadmap.md](../roadmap.md) and [implementation_progress.md](../implementation_progress.md) |
| [risk_register_2026-04.md](risk_register_2026-04.md) | The April risk register from `roadmap/` | the open rows of [review_findings_tracker.md](../implementation_progress/review_findings_tracker.md) |

Moved here on 2026-09-29, unchanged. The top-level `roadmap/` directory held the
last two and no longer exists: one place for documentation is easier to keep true
than two.
