type Page = { data: { privateMetadata: Record<string, unknown> }[]; totalCount: number };

// How many accounts hold a license. Clerk can't filter on private metadata, so every account is read, a page at a time.
// ponytail: a few requests per thousand accounts, every 10 minutes (StatsController's cache); past ~50k accounts keep a
// running count where the license is issued instead
export async function countLicenses(page: (offset: number) => Promise<Page>): Promise<number> {
  let count = 0;
  for (let offset = 0; ; ) {
    const { data, totalCount } = await page(offset);
    count += data.filter((u) => typeof u.privateMetadata.license === 'string').length;
    offset += data.length;
    if (!data.length || offset >= totalCount) return count;
  }
}
