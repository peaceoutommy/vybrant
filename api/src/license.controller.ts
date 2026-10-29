import { Controller, Get, Post, Req, UseGuards } from '@nestjs/common';
import { createClerkClient, type User } from '@clerk/backend';
import { ClerkGuard } from './clerk.guard';
import { loadPrivateKey, signLicense } from './license';

// One license per account, kept in the user's Clerk private metadata (only this server can read or write it)
@Controller('license')
@UseGuards(ClerkGuard)
export class LicenseController {
  private readonly clerk = createClerkClient({ secretKey: process.env.CLERK_SECRET_KEY });
  private readonly key = loadPrivateKey(process.env.LICENSE_PRIVATE_KEY ?? '');

  @Get()
  async get(@Req() req: { userId: string }) {
    const user = await this.clerk.users.getUser(req.userId);
    return { license: (user.privateMetadata.license as string | undefined) ?? null };
  }

  @Post()
  async create(@Req() req: { userId: string }) {
    const user = await this.clerk.users.getUser(req.userId);
    const existing = user.privateMetadata.license as string | undefined;
    if (existing) return { license: existing };
    const license = signLicense({ v: 1, sub: user.id, name: displayName(user), email: email(user) }, this.key);
    await this.clerk.users.updateUserMetadata(user.id, { privateMetadata: { license } });
    return { license };
  }
}

function email(user: User): string {
  return user.primaryEmailAddress?.emailAddress ?? user.emailAddresses[0]?.emailAddress ?? '';
}
function displayName(user: User): string {
  return [user.firstName, user.lastName].filter(Boolean).join(' ') || user.username || email(user);
}
