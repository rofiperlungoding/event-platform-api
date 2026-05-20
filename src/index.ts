import Fastify from 'fastify';
import { PrismaClient, Prisma } from '@prisma/client';
import { z } from 'zod';

const app = Fastify({
  logger: {
    transport: { target: 'pino-pretty' },
  },
});

const prisma = new PrismaClient();

// ─── Schemas ────────────────────────────────────────────────────────────────
const RegisterSchema = z.object({
  name: z.string().min(2).max(100),
  email: z.string().email(),
  team: z.string().min(1).max(100),
});

const ParamsIdSchema = z.object({
  id: z.coerce.number().int().positive(),
});

// ─── Routes ─────────────────────────────────────────────────────────────────
app.get('/health', async () => ({ status: 'ok', uptime: process.uptime() }));

app.get('/participants', async () => {
  return prisma.participant.findMany({ orderBy: { createdAt: 'desc' } });
});

app.get('/participants/:id', async (req, reply) => {
  const parsed = ParamsIdSchema.safeParse(req.params);
  if (!parsed.success) return reply.status(400).send({ error: parsed.error.flatten() });

  const participant = await prisma.participant.findUnique({ where: { id: parsed.data.id } });
  if (!participant) return reply.status(404).send({ error: 'Participant not found' });
  return participant;
});

app.post('/register', async (req, reply) => {
  const parsed = RegisterSchema.safeParse(req.body);
  if (!parsed.success) return reply.status(400).send({ error: parsed.error.flatten() });

  try {
    const participant = await prisma.participant.create({ data: parsed.data });
    return reply.status(201).send(participant);
  } catch (err) {
    if (err instanceof Prisma.PrismaClientKnownRequestError && err.code === 'P2002') {
      return reply.status(409).send({ error: 'Email already registered' });
    }
    throw err;
  }
});

app.delete('/participants/:id', async (req, reply) => {
  const parsed = ParamsIdSchema.safeParse(req.params);
  if (!parsed.success) return reply.status(400).send({ error: parsed.error.flatten() });

  try {
    await prisma.participant.delete({ where: { id: parsed.data.id } });
    return reply.status(204).send();
  } catch (err) {
    if (err instanceof Prisma.PrismaClientKnownRequestError && err.code === 'P2025') {
      return reply.status(404).send({ error: 'Participant not found' });
    }
    throw err;
  }
});

// ─── Bootstrap ──────────────────────────────────────────────────────────────
const port = Number(process.env.PORT ?? 3000);
const host = '0.0.0.0';

const start = async () => {
  try {
    await app.listen({ port, host });
    app.log.info(`Server listening on http://${host}:${port}`);
  } catch (err) {
    app.log.error(err);
    process.exit(1);
  }
};

const shutdown = async (signal: string) => {
  app.log.info(`${signal} received, shutting down...`);
  await app.close();
  await prisma.$disconnect();
  process.exit(0);
};

process.on('SIGINT', () => shutdown('SIGINT'));
process.on('SIGTERM', () => shutdown('SIGTERM'));

start();
