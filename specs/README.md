# specs

Day Shift planning documents for Nightmanager.

Rules:
- Use `TEMPLATE.md` for new specs.
- Prefix unfinished specs `draft-`; Nightmanager ignores them.
- Promote TODOs to `[ready]` only when linked to a complete non-draft spec; `to-ready` can promote reviewed drafts.
- Specs optimize human thinking first; good specs reduce agent babysitting.

## Draft specs

- Filename: `draft-<title>.md`
- TODOs linked to drafts stay `[draft]`
- Human review + `to-ready`: remove `draft-`, set `Status: active`, mark TODOs `[ready]`, make one promotion commit

## Readiness Checklist

- Problem/desired behavior clear
- Scope small enough for one Nightmanager TODO
- Acceptance criteria testable
- Edge cases/non-goals documented
- Includes `## Testing Plan`
- Open questions resolved or deferred
