import { FastifyInstance } from 'fastify';
import os from 'node:os';
import { execSync } from 'node:child_process';
import { PrismaClient } from '../generated/prisma/client';

export function registerSystemRoutes(app: FastifyInstance, prisma: PrismaClient) {
  // ─── Detailed health ──────────────────────────────────────────────────────
  app.get('/health/detailed', async () => {
    const start = Date.now();
    let dbOk = false;
    let dbLatency = -1;
    try {
      const t = Date.now();
      await prisma.$queryRawUnsafe('SELECT 1');
      dbLatency = Date.now() - t;
      dbOk = true;
    } catch {
      dbOk = false;
    }

    return {
      status: dbOk ? 'healthy' : 'degraded',
      timestamp: new Date().toISOString(),
      checks: {
        api: { status: 'ok', latency_ms: Date.now() - start },
        database: {
          status: dbOk ? 'ok' : 'error',
          latency_ms: dbLatency,
        },
      },
      uptime_seconds: Math.floor(process.uptime()),
      version: process.env.npm_package_version ?? '0.1.0',
      node_version: process.version,
    };
  });

  // ─── System metrics (CPU, memory, load) ───────────────────────────────────
  app.get('/system', async () => {
    const totalMem = os.totalmem();
    const freeMem = os.freemem();
    const usedMem = totalMem - freeMem;

    const cpus = os.cpus();
    const loadAvg = os.loadavg();

    // Process memory
    const procMem = process.memoryUsage();

    return {
      hostname: os.hostname(),
      platform: os.platform(),
      arch: os.arch(),
      cpu: {
        model: cpus[0]?.model ?? 'unknown',
        cores: cpus.length,
        speed_mhz: cpus[0]?.speed ?? 0,
        load_avg: {
          '1m': loadAvg[0],
          '5m': loadAvg[1],
          '15m': loadAvg[2],
        },
      },
      memory: {
        total_bytes: totalMem,
        used_bytes: usedMem,
        free_bytes: freeMem,
        used_percent: Number(((usedMem / totalMem) * 100).toFixed(2)),
      },
      process: {
        rss_bytes: procMem.rss,
        heap_total_bytes: procMem.heapTotal,
        heap_used_bytes: procMem.heapUsed,
        external_bytes: procMem.external,
      },
      uptime: {
        system_seconds: Math.floor(os.uptime()),
        process_seconds: Math.floor(process.uptime()),
      },
      timestamp: new Date().toISOString(),
    };
  });

  // ─── Database stats ────────────────────────────────────────────────────────
  app.get('/stats/database', async () => {
    try {
      const [dbInfo, tableStats, participantCount] = await Promise.all([
        prisma.$queryRawUnsafe<Array<{ version: string; size: bigint }>>(
          `SELECT version() AS version, pg_database_size(current_database()) AS size`,
        ),
        prisma.$queryRawUnsafe<Array<{ relname: string; n_live_tup: bigint; pg_size: bigint }>>(
          `SELECT relname, n_live_tup, pg_total_relation_size(relid) AS pg_size
           FROM pg_stat_user_tables
           ORDER BY n_live_tup DESC`,
        ),
        prisma.participant.count(),
      ]);

      return {
        database: {
          version: dbInfo[0]?.version.split(' on ')[0] ?? 'unknown',
          size_bytes: Number(dbInfo[0]?.size ?? 0),
        },
        tables: tableStats.map((t) => ({
          name: t.relname,
          row_count: Number(t.n_live_tup),
          size_bytes: Number(t.pg_size),
        })),
        totals: {
          participants: participantCount,
        },
        timestamp: new Date().toISOString(),
      };
    } catch (err) {
      return { error: 'Failed to fetch database stats', detail: String(err) };
    }
  });

  // ─── App stats / aggregations ─────────────────────────────────────────────
  app.get('/stats/participants', async () => {
    const total = await prisma.participant.count();
    const byTeam = await prisma.participant.groupBy({
      by: ['team'],
      _count: { _all: true },
      orderBy: { _count: { team: 'desc' } },
    });

    const recent = await prisma.participant.findMany({
      orderBy: { createdAt: 'desc' },
      take: 5,
      select: { id: true, name: true, email: true, team: true, createdAt: true },
    });

    return {
      total,
      by_team: byTeam.map((t) => ({ team: t.team, count: t._count._all })),
      recent_5: recent,
      timestamp: new Date().toISOString(),
    };
  });
}
