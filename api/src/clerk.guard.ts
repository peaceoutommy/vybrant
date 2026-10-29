import { CanActivate, ExecutionContext, Injectable, UnauthorizedException } from '@nestjs/common';
import { verifyToken } from '@clerk/backend';

// Requests carry the website's Clerk session token ("Authorization: Bearer ..."); a valid one puts the user id on the request
@Injectable()
export class ClerkGuard implements CanActivate {
  async canActivate(context: ExecutionContext): Promise<boolean> {
    const req = context.switchToHttp().getRequest();
    const token = /^Bearer (.+)$/.exec(req.headers.authorization ?? '')?.[1];
    if (!token) throw new UnauthorizedException();
    try {
      const claims = await verifyToken(token, { secretKey: process.env.CLERK_SECRET_KEY, authorizedParties: [process.env.WEB_ORIGIN!] });
      req.userId = claims.sub;
      return true;
    } catch {
      throw new UnauthorizedException();
    }
  }
}
