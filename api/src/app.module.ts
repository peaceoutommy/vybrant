import { Module } from '@nestjs/common';
import { LicenseController } from './license.controller';
import { StatsController } from './stats.controller';

@Module({ controllers: [LicenseController, StatsController] })
export class AppModule {}
