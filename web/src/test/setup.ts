import '@testing-library/jest-dom/vitest'

const hostZone = Intl.DateTimeFormat().resolvedOptions().timeZone

if (hostZone !== 'UTC') {
  throw new Error(
    `Tests require TZ=UTC; this process resolved "${hostZone}". A timezone test run in the ` +
      `zone under test passes whether or not the code passes the zone through. Run "npm test".`,
  )
}
