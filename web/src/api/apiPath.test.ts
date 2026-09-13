import { describe, expect, it } from 'vitest'
import { API_PREFIX, apiUrl, daemonPath } from './apiPath'

describe('apiUrl', () => {
  it('prefixes a daemon route with the proxy mount point', () => {
    expect(apiUrl('/status')).toBe('/api/status')
    expect(apiUrl('/zones/3/run')).toBe('/api/zones/3/run')
  })

  it('is the only place the mount point is spelled', () => {
    expect(API_PREFIX).toBe('/api')
  })
})

describe('daemonPath', () => {
  it('rewrites the proxy prefix to the daemon prefix', () => {
    expect(daemonPath('/api/status')).toBe('/admin/status')
    expect(daemonPath('/api/zones/3/run')).toBe('/admin/zones/3/run')
    expect(daemonPath('/api/programs/12')).toBe('/admin/programs/12')
  })

  it('leaves a nested "api" segment alone', () => {
    expect(daemonPath('/api/zones/api/run')).toBe('/admin/zones/api/run')
  })

  it('rejects a url that is already a daemon path', () => {
    expect(() => daemonPath('/admin/status')).toThrow(/api/)
  })
})
