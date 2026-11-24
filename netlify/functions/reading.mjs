import { getStore } from "@netlify/blobs";

const store = getStore("alcoholimetro-data");
const cors = {
  "Access-Control-Allow-Origin": "*",
  "Access-Control-Allow-Headers": "Content-Type, X-Device-Token",
  "Access-Control-Allow-Methods": "GET, POST, OPTIONS",
  "Content-Type": "application/json; charset=utf-8",
  "Cache-Control": "no-store"
};

const defaultConfig = {
  mode: "auto",          // auto | silent | test
  warning: 35,           // índice relativo 0..100
  high: 65,
  critical: 85,
  alarmEnabled: true,
  warmupHours: 48,
  warmupCommand: 0
};

const json = (data, status = 200) =>
  new Response(JSON.stringify(data), { status, headers: cors });

export default async (request) => {
  if (request.method === "OPTIONS") return new Response(null, { status: 204, headers: cors });

  try {
    if (request.method === "GET") {
      const [readingRaw, configRaw] = await Promise.all([
        store.get("latest", { type: "json" }),
        store.get("config", { type: "json" })
      ]);
      return json({
        reading: readingRaw || null,
        config: { ...defaultConfig, ...(configRaw || {}) },
        serverTime: new Date().toISOString()
      });
    }

    if (request.method !== "POST") return json({ error: "Método no permitido" }, 405);

    const body = await request.json();

    // El panel inicia un nuevo ciclo físico de precalentamiento.
    if (body.action === "startWarmup") {
      const hours = clamp(body.hours, 1, 48, 48);
      const previous = (await store.get("config", { type: "json" })) || {};
      const cfg = { ...defaultConfig, ...previous, warmupHours: hours, warmupCommand: Date.now() };
      await store.set("config", JSON.stringify(cfg));
      return json({ ok: true, config: cfg });
    }

    // Configuración enviada desde el panel. Se limita a valores conocidos.
    if (body.action === "config") {
      const cfg = {
        mode: ["auto", "silent", "test"].includes(body.config?.mode) ? body.config.mode : "auto",
        warning: clamp(body.config?.warning, 5, 90, defaultConfig.warning),
        high: clamp(body.config?.high, 10, 95, defaultConfig.high),
        critical: clamp(body.config?.critical, 15, 100, defaultConfig.critical),
        alarmEnabled: Boolean(body.config?.alarmEnabled),
        warmupHours: clamp(body.config?.warmupHours, 1, 48, defaultConfig.warmupHours),
        warmupCommand: Number(body.config?.warmupCommand || 0)
      };
      if (!(cfg.warning < cfg.high && cfg.high < cfg.critical)) {
        return json({ error: "Los niveles deben cumplir advertencia < alta < crítica." }, 400);
      }
      await store.set("config", JSON.stringify(cfg));
      return json({ ok: true, config: cfg });
    }

    const expected = Netlify.env.get("DEVICE_TOKEN");
    const received = request.headers.get("x-device-token");
    if (!expected || received !== expected) return json({ error: "No autorizado" }, 401);

    const value = Number(body.raw);
    const index = Number(body.index);
    const warming = Boolean(body.warming);
    if (!Number.isFinite(value) || value < 0 || value > 1023 ||
        !Number.isFinite(index) || index < 0 || index > 100) {
      return json({ error: "Lectura inválida" }, 400);
    }

    const reading = {
      raw: Math.round(value),
      index: Math.round(index * 10) / 10,
      mgL: !warming && body.calibrated && Number.isFinite(Number(body.mgL)) ? Number(body.mgL) : null,
      calibrated: !warming && Boolean(body.calibrated),
      warming,
      warmupHours: clamp(body.warmupHours, 1, 48, 48),
      remainingSec: clamp(body.remainingSec, 0, 48 * 3600, 0),
      status: String(warming ? "Precalentando" : (body.status || "Sin clasificar")).slice(0, 24),
      deviceTime: String(body.deviceTime || "").slice(0, 40),
      receivedAt: new Date().toISOString()
    };
    await store.set("latest", JSON.stringify(reading));
    return json({ ok: true, receivedAt: reading.receivedAt });
  } catch (error) {
    return json({ error: "Error procesando la solicitud", detail: String(error?.message || error) }, 500);
  }
};

function clamp(value, min, max, fallback) {
  const n = Number(value);
  return Number.isFinite(n) ? Math.min(max, Math.max(min, n)) : fallback;
}
