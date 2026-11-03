# Spec Delta

## Purpose

Adds to control-plane/policy-service the adoption report that the admin console shows for a published or rolled-out policy version.

## ADDED Requirements

### Requirement: Rollout Adoption
The policy service SHALL report, for a Group or the guardrail, how many active installations run each published version of its policy document. An installation runs a version when the Policy bundle it last reported to control-plane/fleet-registry is the bundle the policy service last issued to its user, compiled with that version. Other installations are counted as not yet updated.

#### Scenario: Adoption during a pilot rollout
- **WHEN** a version is published to a pilot Group and the pilot users' installations fetch and report their new bundles
- **THEN** the report counts those installations under the new version and the other installations under the version in force, and gives the number of users the rollout selected

#### Scenario: Installation not yet updated
- **WHEN** an installation still reports a bundle older than the one last issued to its user
- **THEN** the report counts it as not yet updated
