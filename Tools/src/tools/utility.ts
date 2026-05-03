import type { McpServer } from "@modelcontextprotocol/sdk/server/mcp.js";
import { z } from "zod";
import { ensureUE, ueGet, uePost, getUEHealth, isUEHealthy, gracefulShutdown, state } from "../ue-bridge.js";

export function registerUtilityTools(server: McpServer): void {
  server.tool(
    "server_status",
    "Report UE5 Blueprint server reachability. Read-only — does NOT spawn a commandlet, does NOT block waiting for startup. If the server is not currently reachable, says so and notes that other tool calls will spawn one. Returns: running (editor/commandlet) with index counts, or starting (a spawn is already in flight), or not running.",
    {},
    async () => {
      // Probe directly. ensureUE() is intentionally NOT called: this is a status
      // query, and triggering a 3-minute commandlet spawn wait from a status check
      // is the wrong default. If the server is up, report it; if not, say so.
      const health = await getUEHealth();
      if (health) {
        // Side effect: if a stale spawn is still registered, drop it. The fact
        // that /api/health responds means whatever process answers (editor or a
        // commandlet we didn't spawn) is good — clear the stale handle so the
        // next ensureUE() doesn't try to kill it on line 244.
        if (state.ueProcess && health.mode === "editor") {
          state.ueProcess = null;
        }
        state.editorMode = health.mode === "editor";
        return {
          content: [{
            type: "text" as const,
            text: `UE5 Blueprint server is running (${health.mode} mode).\nBlueprints indexed: ${health.blueprintCount}\nMaps indexed: ${health.mapCount ?? "?"}`,
          }],
        };
      }

      if (state.startupPromise) {
        return {
          content: [{
            type: "text" as const,
            text: "UE5 Blueprint server is starting (a commandlet spawn is in flight). Retry server_status in ~30s, or call any blueprint tool to wait on the same startup.",
          }],
        };
      }

      return {
        content: [{
          type: "text" as const,
          text: "UE5 Blueprint server is not reachable on port 9847. Open the editor (preferred) or call any blueprint tool to spawn a headless commandlet.",
        }],
      };
    }
  );

  server.tool(
    "rescan_assets",
    "Re-scan the UE5 asset registry and refresh the server's cached asset lists. Use this if newly created assets are not appearing in list_blueprints/list_materials, or if the server started before the editor finished loading assets.",
    {},
    async () => {
      const err = await ensureUE();
      if (err) return { content: [{ type: "text" as const, text: err }] };

      const data = await uePost("/api/rescan", {});
      if (data.error) {
        return { content: [{ type: "text" as const, text: `Rescan failed: ${data.error}` }] };
      }

      const lines = [
        "Asset registry rescanned.",
        `Blueprints: ${data.blueprintCount}${data.delta?.blueprints ? ` (${data.delta.blueprints >= 0 ? "+" : ""}${data.delta.blueprints})` : ""}`,
        `Maps: ${data.mapCount}${data.delta?.maps ? ` (${data.delta.maps >= 0 ? "+" : ""}${data.delta.maps})` : ""}`,
        `Materials: ${data.materialCount}${data.delta?.materials ? ` (${data.delta.materials >= 0 ? "+" : ""}${data.delta.materials})` : ""}`,
        `Material Instances: ${data.materialInstanceCount}${data.delta?.materialInstances ? ` (${data.delta.materialInstances >= 0 ? "+" : ""}${data.delta.materialInstances})` : ""}`,
        `Material Functions: ${data.materialFunctionCount}${data.delta?.materialFunctions ? ` (${data.delta.materialFunctions >= 0 ? "+" : ""}${data.delta.materialFunctions})` : ""}`,
      ];
      return { content: [{ type: "text" as const, text: lines.join("\n") }] };
    }
  );

  server.tool(
    "exec_command",
    "Execute an editor console command and return its output. Requires editor mode (not commandlet). Useful for: saving assets (\"Asset.SaveAll\"), running automation tests (\"Automation RunTests <filter>\"), triggering Live Coding, etc.",
    {
      command: z.string().describe("The console command to execute (e.g. \"Asset.SaveAll\", \"Automation RunTests MyTests\")"),
    },
    async ({ command }) => {
      const err = await ensureUE();
      if (err) return { content: [{ type: "text" as const, text: err }] };

      const data = await uePost("/api/exec", { command });
      if (data.error) {
        return { content: [{ type: "text" as const, text: `Error: ${data.error}` }] };
      }

      const lines = [
        `Command: ${data.command}`,
        `Success: ${data.success}`,
      ];
      if (data.output) {
        lines.push(`Output:\n${data.output}`);
      }
      return { content: [{ type: "text" as const, text: lines.join("\n") }] };
    }
  );

  server.tool(
    "run_python",
    "Execute Python in the editor and return captured stdout/stderr/exception in one round-trip. Replaces the `exec_command(\"py <path>\") + Read(Saved/python_test.txt)` boilerplate. Pass either inline code or a path to a .py file (with optional positional args) — file detection is automatic for `.py` extensions. Uncaught exceptions land in the response (`success=false`, traceback in `result`, error entries in `log`); no need to wrap probes in try/except just for capture. Editor-only — Python module must be loaded.",
    {
      script: z.string().describe("Inline Python code OR path to a .py file (with optional positional args, e.g. \"C:/path/to/probe.py arg1 arg2\"). The `.py` extension auto-routes to file execution; otherwise treated as literal code."),
      mode: z.enum(["file", "statement", "eval"]).optional().describe("Execution mode. \"file\" (default) handles both inline scripts and .py files. \"statement\" runs a single Python statement. \"eval\" evaluates a single expression and returns its value in `result`."),
      unattended: z.boolean().optional().describe("Run with GIsRunningUnattendedScript=true to suppress modal UI prompts. Default true — set false only when the script needs interactive dialogs."),
    },
    async ({ script, mode, unattended }) => {
      const err = await ensureUE();
      if (err) return { content: [{ type: "text" as const, text: err }] };

      const body: Record<string, unknown> = { script };
      if (mode !== undefined) body.mode = mode;
      if (unattended !== undefined) body.unattended = unattended;

      const data = await uePost("/api/run-python", body);
      if (data.error) {
        return { content: [{ type: "text" as const, text: `Error: ${data.error}` }] };
      }

      const lines: string[] = [
        `Success: ${data.success}`,
        `Mode: ${data.mode}`,
      ];
      if (typeof data.errorCount === "number" && data.errorCount > 0) {
        lines.push(`Errors: ${data.errorCount}`);
      }
      if (typeof data.warningCount === "number" && data.warningCount > 0) {
        lines.push(`Warnings: ${data.warningCount}`);
      }
      if (data.result) {
        lines.push("", "Result:", String(data.result));
      }
      if (Array.isArray(data.log) && data.log.length > 0) {
        lines.push("", "Log:");
        for (const entry of data.log) {
          const type = entry?.type ?? "?";
          const text = entry?.output ?? "";
          lines.push(`  [${type}] ${text}`);
        }
      }
      return { content: [{ type: "text" as const, text: lines.join("\n") }] };
    }
  );

  server.tool(
    "shutdown_server",
    "Shut down the UE5 Blueprint server to free memory (~2-4 GB). The server will auto-restart on the next blueprint tool call. Use this when done with blueprint analysis. Cannot shut down the editor — only the standalone commandlet.",
    {},
    async () => {
      if (state.editorMode) {
        return {
          content: [{
            type: "text" as const,
            text: "Connected to UE5 editor \u2014 cannot shut down the editor's MCP server. Close the editor to stop serving.",
          }],
        };
      }

      if (!state.ueProcess && !state.startupPromise && !(await isUEHealthy())) {
        return { content: [{ type: "text" as const, text: "UE5 server is already stopped." }] };
      }

      await gracefulShutdown();
      state.startupPromise = null;

      return {
        content: [{
          type: "text" as const,
          text: "UE5 Blueprint server shut down. Memory freed. It will auto-restart on the next blueprint tool call.",
        }],
      };
    }
  );
}
