import { Controller, Get } from '@nestjs/common';
import { createClerkClient } from '@clerk/backend';
import { countLicenses } from './stats';

const TTL = 10 * 60_000;

// Public: the number behind the site's "Join N users". Counted at most every 10 minutes; requests in between share it.
@Controller('stats')
export class StatsController {
  private readonly clerk = createClerkClient({ secretKey: process.env.CLERK_SECRET_KEY });
  private licenses: Promise<number> | null = null;
  private at = 0;

  @Get()
  async get() {
    if (!this.licenses || Date.now() - this.at > TTL) {
      this.at = Date.now();
      this.licenses = countLicenses((offset) => this.clerk.users.getUserList({ limit: 500, offset })).catch((e) => {
        this.licenses = null;   // the next request tries again
        throw e;
      });
    }
    return { licenses: await this.licenses };
  }
}
