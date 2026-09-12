# Extend existing systems before creating new ones

Treat a request for a capability as a request for the user-visible outcome, not
as an instruction to build a new subsystem. Do not assume the user knows which
models, tools, actions, editors, or workflows already implement it.

Before implementing a feature or fixing an apparently missing feature:

- Trace the current workflow and search for the existing capability, including
  its model, service/API, editor, actions, persistence, validation, and undo path.
- Prefer connecting, exposing, configuring, or repairing that existing system.
  A new button or shortcut should normally call the shared action with the right
  context or destination, using only the glue needed to connect the workflow.
- If support is missing, extend the shared system at the appropriate boundary.
  Do not introduce a parallel implementation, one-off feature type, duplicate
  state, alternate settings panel, or competing workflow for a single use case.
- Before creating a new mechanism, establish a concrete requirement the existing
  system cannot meet and explain why a shared extension is insufficient. An
  awkward API or missing UI connection alone does not justify duplication.
- Keep existing projects compatible through conversion when replacing old
  behavior. Do not leave two competing authoring systems exposed to users.
- Verify the actual user workflow through the existing tools, including that
  created items appear in their normal management interface and can be edited,
  removed, persisted, and undone there.

These rules apply across the repository and across feature areas. For example,
LFO shortcuts use the normal `ParameterLfo` model, creation path, and Numeric LFO
editor; they do not create a separate oscillator implementation per destination.

# macOS support policy

PVT supports macOS 27.0 and later only. Do not lower the deployment target,
restore older-OS fallbacks, or pin dependencies to preserve old macOS support.
Use the current macOS SDK and system VideoToolbox for Mac video encoding.
