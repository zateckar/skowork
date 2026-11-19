# Proposal

## Why

Two red-team tests failed on 2026-11-02: an injected request to delete a file, to run an undeclared tool or to run a shell command left no trace in the Run. Policy withholds those tools (Withheld Tool Explanation), and a direct call of a withheld tool returned the explanation to the model but was not recorded as a decision. The user and the audit could not see that the agent tried, and what stopped it.

## What Changes

- A direct call of a withheld tool is recorded in the Run as a denied call, with the explanation (the capability and the policy key) as the reason, as for every other denied call. The model still gets the same explanation.

## Capabilities

### New Capabilities
- None.

### Modified Capabilities
- `client/policy-enforcement`: Withheld Tool Explanation (modified).

## Impact

- **Roadmap:** 26. A fix to improve-usability (Withheld Tool Explanation) and the computer-use fix of 2026-11-02 (4210d3c).
- **Code:** client/src/runtime (the decision record of a withheld call).
- **Tests:** RedTeam.InjectedExternalAndDestructiveActionsNeverRunWithoutTheUser and RedTeam.RemoteTaskPromptCannotWidenRightsInAnUnattendedRun.
- **Data:** none.
