# AGENTS.md

## Purpose

Repository guidance for coding agents working in this repo.

## Default Workflow

1. Make the requested changes.
2. Run the most relevant verification available for the changed area when local tooling is available.
3. Create a git commit for the requested changes.
4. Push the commit to `origin` on the current branch.

## Push Policy

- After a successful change, agents should push to `origin` by default, even when local verification is unavailable.
- Repository-side GitHub Actions, code scanning, and other remote checks are the primary verification backstop when local verification cannot be run.
- Name the commit based on what changed so the history is easy to scan.
- When the work is for a release, include a short release-oriented description in the commit message or accompanying summary so the purpose of the release is clear.
- Use a normal non-interactive flow:
  - `git add ...`
  - `git commit -m "<what changed>: <short release description if applicable>"`
  - `git push origin HEAD`
- If the push is rejected because the remote moved, stop and report it instead of force-pushing.
- Never use `git push --force` or `git push --force-with-lease` unless the user explicitly asks for it.

## Safety Rules

- Do not push if local verification clearly failed for the changed area and indicates the change is broken.
- If local verification is unavailable, note that clearly and still push unless the user says otherwise.
- Do not revert unrelated user changes.
- Do not amend existing commits unless the user explicitly asks.
- If credentials, branch protections, or remote permissions block the push, report the blocker clearly.
- The legacy fixed-length string copy helper is banned. Use `strlcpy` or an explicit bounded copy pattern instead.
- Placeholder or no-op implementations are banned in project-owned code. Implement real behavior or return an explicit unsupported error, and keep `make -f Makefile.multiarch check-source-complete` passing.

## Communication

- In the final response, state whether the agent:
  - changed files,
  - ran verification,
  - created a commit,
  - pushed to `origin`.
