# ports/linux: rules for an agent working here

- Read `DEVELOPMENT.md` first: layout, build, what bit us, the plan for GPIO.
- Commits in this fork carry NO Co-Authored-By, Claude-Session or similar trailers.
- Never push or open pull requests; the user does.
- A reset on Windows nests a process. Run the Windows program under a process-count limit, as
  `tests/test_wine.py` and `tests/test_dap.py` do, and kill leftovers by PID. Never wait on or
  kill by a pattern that your own command line contains (`pkill -f`, `pgrep -f`).
- After a change to `mpconfigport.h` or `mpconfigport.mk`, delete the build directory.
- Build the Linux and the Windows program before committing C changes; run `tests/`.
- Keep code comments to what and at most one why; verification belongs in the commit message.
