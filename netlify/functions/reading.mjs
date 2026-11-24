
import { getStore } from "@netlify/blobs";

const headers = {
  "Access-Control-Allow-Origin": "*",
  "Access-Control-Allow-Headers": "Content-Type",
  "Access-Control-Allow-Methods": "GET, POST, OPTIONS",
  "Content-Type": "application/json; charset=utf-8",
  "Cache-Control": "no-store"
};

const defaults = {
  mode: "auto",
  warning: 35,
  high: 65,
  critical: 85,
  warmupHours: 48,
  warmupCommand: 0
};

const reply = (data, status = 200) =>
  new Response(JSON.stringify(data), { status, headers });

const clamp = (v, min, max, fallback) =>
  Number.isFinite(Number(v))
    ? Math.min(max, Math.max(min, Number(v)))
    : fallback;

export default async (request) => {

  if (request.method === "OPTIONS") {
    return new Response(null, { status: 204, headers });
  }

  try {

    // IMPORTANTE:
    // Crear una instancia nueva en cada solicitud.
    // No mover esta línea fuera de la función.
    const store = getStore("alcoholimetro-data");

    if (request.method === "GET") {

      const [reading, saved] = await Promise.all([
        store.get("latest", { type: "json" }),
        store.get("config", { type: "json" })
      ]);

      return reply({
        reading: reading || null,
        config: { ...defaults, ...(saved || {}) },
        serverTime: new Date().toISOString()
      });
    }

    if (request.method !== "POST") {
      return reply({ error: "Método no permitido" }, 405);
    }

    const body = await request.json();

    const old = {
      ...defaults,
      ...((await store.get("config", { type: "json" })) || {})
    };

    // PRECALENTAMIENTO
    if (body.action === "startWarmup") {

      const hours = Math.round(
        clamp(body.hours, 1, 48, 48)
      );

      const config = {
        ...old,
        warmupHours: hours,
        warmupCommand: Date.now()
      };

      await store.set("config", JSON.stringify(config));

      return reply({ ok: true, config });
    }

    // CONFIGURACION DE ALARMAS
    if (body.action === "config") {

      const incoming = body.config || {};

      const config = {
        ...old,

        mode: ["auto", "silent", "test"].includes(incoming.mode)
          ? incoming.mode
          : old.mode,

        warning: clamp(incoming.warning, 5, 90, old.warning),
        high: clamp(incoming.high, 10, 95, old.high),
        critical: clamp(incoming.critical, 15, 100, old.critical)
      };

      if (!(config.warning < config.high &&
            config.high < config.critical)) {

        return reply({
          error: "Los niveles deben cumplir advertencia < alta < crítica."
        }, 400);
      }

      await store.set("config", JSON.stringify(config));

      return reply({ ok: true, config });
    }

    // TELEMETRIA DEL ESP8266
    const raw = Number(body.raw);
    const index = Number(body.index);
    const warming = Boolean(body.warming);

    if (
      !Number.isFinite(raw) ||
      raw < 0 ||
      raw > 1023 ||
      !Number.isFinite(index) ||
      index < 0 ||
      index > 100
    ) {
      return reply({ error: "Lectura inválida" }, 400);
    }

    const reading = {

      raw: Math.round(raw),

      index: Math.round(index * 10) / 10,

      mgL:
        !warming &&
        body.calibrated &&
        Number.isFinite(Number(body.mgL))
          ? Number(body.mgL)
          : null,

      calibrated: !warming && Boolean(body.calibrated),

      warming,

      warmupHours: Math.round(
        clamp(body.warmupHours, 1, 48, 48)
      ),

      remainingSec: Math.round(
        clamp(body.remainingSec, 0, 172800, 0)
      ),

      status: String(
        warming
          ? "Precalentando"
          : (body.status || "Sin clasificar")
      ).slice(0, 24),

      receivedAt: new Date().toISOString()
    };

    await store.set("latest", JSON.stringify(reading));

    return reply({
      ok: true,
      receivedAt: reading.receivedAt
    });

  } catch (error) {

    console.error("VitaSense error:", error);

    return reply({
      error: "Error procesando la solicitud",
      detail: String(error?.message || error)
    }, 500);
  }
};
