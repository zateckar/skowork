// control-plane/api-connectors Schema Budget: a Group's connectors hold at most 512 KB of selected input schemas in total.
// Checked when a selection or an assignment is saved, never when a bundle is built (design D9).
import type { Db } from '../db.ts';
import { ApiError } from '../http.ts';

export const GROUP_SCHEMA_BUDGET_BYTES = 512 * 1024;

/**
 * Refuses the save when any of `groups` would hold more than the budget with this connector's selected schemas.
 * `bytes` is the connector's selected schema size after the save; by default the stored one is used.
 */
export function checkSchemaBudget(db: Db, connectorId: string, groups: string[], bytes?: number): void {
  const mine =
    bytes ??
    (db.prepare("SELECT COALESCE(SUM(schema_bytes), 0) AS n FROM api_operations WHERE connector_id = ? AND status = 'selected' AND usable = 1").get(connectorId) as { n: number }).n;
  for (const group of groups) {
    const others = (db
      .prepare(
        `SELECT COALESCE(SUM(o.schema_bytes), 0) AS n FROM api_operations o
         JOIN mcp_servers s ON s.id = o.connector_id
         JOIN mcp_assignments a ON a.server_id = s.id
         WHERE o.status = 'selected' AND o.usable = 1 AND s.enabled = 1 AND o.connector_id <> ? AND a.skowork_group = ?`,
      )
      .get(connectorId, group) as { n: number }).n;
    if (others + mine > GROUP_SCHEMA_BUDGET_BYTES) {
      throw new ApiError(400, 'BUDGET_EXCEEDED', `The Group ${group} would hold ${others + mine} bytes of connector schemas; the limit is ${GROUP_SCHEMA_BUDGET_BYTES} bytes.`, {
        field: 'groups',
        reason: 'schema budget',
      });
    }
  }
}
