# Spec Delta

## Purpose

Defines how the client isolates every process it starts on the agent's behalf (shell commands, skill scripts, stdio MCP servers and ML workers), limits their resources, and applies the network policy to their traffic.

## ADDED Requirements

### Requirement: Contained Child Processes
The client SHALL start every child process it launches on the agent's behalf inside a containment group. The group terminates all its processes when the client exits or the owning Run is cancelled, and MUST NOT allow processes to break away from it.

#### Scenario: Client exits
- **WHEN** the client process exits while a shell command started by a Run is still running
- **THEN** that command and every process it spawned are terminated

#### Scenario: Breakaway attempt
- **WHEN** a sandboxed process tries to start a child outside its containment group
- **THEN** the child is created inside the group or the creation fails

### Requirement: Resource Limits
The client SHALL apply limits to each containment group: memory, CPU rate, number of active processes, and wall-clock time. The limits come from policy.tools.<group>.limits for the tool group that started it, for example policy.tools.shell.limits. Defaults are memory_mb 1024, cpu_percent 50, processes 32 and wall_minutes 15. A per-call timeout argument can only shorten wall_minutes. Exceeding a limit terminates the group, and the tool returns a limit-exceeded result.

#### Scenario: Runaway memory
- **WHEN** a shell command exceeds its memory limit
- **THEN** its containment group is terminated, and the tool result states that the memory limit was exceeded

#### Scenario: Smaller timeout wins
- **WHEN** a shell.run call asks for a 30-minute timeout and policy.tools.shell.limits.wall_minutes is 15
- **THEN** the command is terminated after 15 minutes

### Requirement: Reduced Privileges
The client SHALL run sandboxed child processes with a restricted token. The token removes administrative group membership and privileges beyond the minimum needed. The process environment contains no Workbench access tokens or other secrets, except a per-process egress proxy credential.

#### Scenario: Token not inherited
- **WHEN** a sandboxed shell command prints its environment
- **THEN** no control-plane or gateway access token appears in the output

#### Scenario: Elevated client
- **WHEN** the client runs with an elevated administrator token, for example with User Account Control off, and starts a sandboxed command
- **THEN** the command starts and runs without administrative group membership, and the objects it creates are owned by the user

### Requirement: Shell Modes
The client SHALL support policy.tools.shell.mode, whose default is `off`, with three values:
- `off`: the shell tool is unavailable;
- `sandboxed`: commands run contained, with reduced privileges, limited to the filesystem rules' working folders and with network only through the egress proxy;
- `allow`: commands run contained with the user's normal privileges.

#### Scenario: Shell off
- **WHEN** policy sets the shell mode to off
- **THEN** `shell.run` is not offered to the model and cannot be found with `find_tools`

#### Scenario: Sandboxed working folder
- **WHEN** a sandboxed command is started with a working folder outside the policy's write rules
- **THEN** the command is not started and a policy-denied result is returned

### Requirement: No-Network Mode
The client SHALL support a no-network mode for sandboxed processes, used when policy or the tool definition requires it, for example a Skill declaring `network: none` (client/skills). The process runs like a sandboxed process that uses the egress proxy, but the proxy refuses every destination for its credential, whatever policy.network allows. The mode uses no operating-system network isolation; its enforcement level is that of Enforcement Limits Disclosure.

#### Scenario: Offline skill script
- **WHEN** a script of a Skill that declares `network: none` requests any host through the proxy
- **THEN** the proxy refuses the request and records the denial in the approval log

#### Scenario: Allowed host still refused
- **WHEN** a no-network process requests a host that matches policy.network.egress_allow
- **THEN** the proxy refuses the request

### Requirement: Egress Proxy For Child Processes
For sandboxed processes that have network access, the client SHALL provide a loopback-only HTTP(S) proxy. The proxy exists only while such processes run, requires a per-process random credential, and applies the policy.network egress rules to every request.

#### Scenario: Allowed package registry
- **WHEN** a sandboxed command requests a host that matches policy.network.egress_allow through the proxy
- **THEN** the request is forwarded

#### Scenario: Blocked host
- **WHEN** a sandboxed command requests a host that is not allowed
- **THEN** the proxy refuses the connection and records the denial in the approval log

#### Scenario: Other local process uses the proxy
- **WHEN** a process without the per-process credential connects to the proxy
- **THEN** the proxy rejects the request

### Requirement: Enforcement Limits Disclosure
The client SHALL show administrators the enforcement level achieved for each Group's network policy, both in policy simulation results and in telemetry. A process that bypasses the proxy can be blocked only by operating-system firewall rules deployed outside Workbench. The levels are:
- `proxy-only`: network policy is applied only to traffic that uses the proxy;
- `os-enforced`: operating-system firewall rules also block traffic that bypasses the proxy.

#### Scenario: No OS firewall profile
- **WHEN** no operating-system firewall profile for Workbench child processes is detected on the machine
- **THEN** the client reports the network enforcement level as `proxy-only`

### Requirement: Output Capture Limits
The client SHALL capture each sandboxed process's standard output and standard error up to policy.tools.<group>.limits.output_kb per stream (default 1024). Output beyond the limit is discarded, and the captured part is passed on as a tool result, subject to the result cap of client/agent-runtime. Output is decoded safely, whatever the console code page.

#### Scenario: Very large output
- **WHEN** a command writes 100 MB to standard output with the default limit
- **THEN** capture stops after 1024 KB, the process is not blocked, and the result notes the truncation
