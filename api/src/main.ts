import 'reflect-metadata';
import { existsSync } from 'node:fs';
import { NestFactory } from '@nestjs/core';

async function bootstrap() {
  if (existsSync('.env')) process.loadEnvFile();   // before the module is loaded: its controller reads the keys
  for (const name of ['CLERK_SECRET_KEY', 'WEB_ORIGIN', 'LICENSE_PRIVATE_KEY'])
    if (!process.env[name]) throw new Error(`${name} is not set (see .env.example)`);
  const { AppModule } = await import('./app.module');
  const app = await NestFactory.create(AppModule);
  app.setGlobalPrefix('api');
  app.enableCors({ origin: process.env.WEB_ORIGIN });
  await app.listen(process.env.PORT ?? 3000);
}
bootstrap();
