# Spec Delta

## Purpose

Defines how the client discovers and diagnoses the organization-managed machine configuration used to connect to the control plane.

## ADDED Requirements

### Requirement: Machine Configuration Sources
The client SHALL use the documented machine registry configuration when its control-plane URL value exists; otherwise it SHALL load the documented JSON configuration beside the running executable, preserving documented defaults and client/identity validation.

#### Scenario: Shortcut starts elsewhere
- **WHEN** the client starts from a different working directory and its installation path contains Unicode characters
- **THEN** it reads the configuration beside its executable and ignores configuration in the working directory

#### Scenario: Registry configuration takes precedence
- **WHEN** the machine registry has a control-plane URL value and an adjacent configuration file also exists
- **THEN** the client validates and uses only the registry configuration, reporting an error if it is invalid

### Requirement: Machine Configuration Names
Each machine configuration source SHALL have a SkoWork name and the Workbench name of earlier deployments. The client MUST use the first that exists in this order: the SkoWork registry key, the Workbench registry key, the SkoWork configuration file, the Workbench configuration file.

#### Scenario: Deployment made under the earlier name
- **WHEN** only the Workbench registry key or only the Workbench configuration file exists
- **THEN** the client loads its settings from it

#### Scenario: Both names exist
- **WHEN** a SkoWork configuration file and a Workbench configuration file are both beside the executable and no registry configuration exists
- **THEN** the client uses only the SkoWork file

### Requirement: Machine Configuration Encoding
The client SHALL accept its machine configuration file in UTF-8 with or without a byte-order mark and UTF-16 in either byte order with a byte-order mark.

#### Scenario: Configuration saved by a Windows editor
- **WHEN** the machine registry has no control-plane URL value and the adjacent configuration is valid JSON saved in any supported encoding
- **THEN** the client loads its settings and uses the documented defaults for omitted optional settings

#### Scenario: Malformed encoding
- **WHEN** the selected configuration contains truncated UTF-16 or invalid Unicode
- **THEN** the client rejects it as invalid configuration

### Requirement: Machine Configuration Diagnostics
When machine configuration cannot be loaded, the client SHALL identify the selected source and distinguish a missing file, an unreadable file, invalid JSON or encoding, and missing required settings. File errors SHALL include the full attempted path without echoing file contents. When the intended file is absent but a file with an extra text-file extension exists beside it, the error SHALL explain the required filename correction without treating that file as configuration.

#### Scenario: Notepad appends a text-file extension
- **WHEN** the documented file is missing but the adjacent file has an extra text-file extension
- **THEN** the error identifies both filenames and tells the administrator to remove the extra extension

#### Scenario: Invalid configuration
- **WHEN** the selected file is malformed, unreadable, or missing required settings
- **THEN** the client stays unconfigured and reports the precise reason and file path without including configuration values
