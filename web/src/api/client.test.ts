import { afterEach, describe, expect, it, vi } from 'vitest'
import {
  createProgram,
  deleteProgram,
  getSettings,
  getStatus,
  getZones,
  putZone,
  runProgram,
  runZone,
  stopAll,
  stopZone,
} from './client'
import { ApiError } from './types'
import { installFetchStub } from '../test/fetchStub'
import { idleStatus, zoneFixtures } from '../test/fixtures'

afterEach(() => {
  vi.unstubAllGlobals()
})

describe('client', () => {
  it('gets status from the proxied path', async () => {
    const { calls } = installFetchStub(() => ({ body: { ...idleStatus, rainDelayUntilUtc: '' } }))
    const status = await getStatus()
    expect(calls[0]?.url).toBe('/api/status')
    expect(calls[0]?.method).toBe('GET')
    expect(status.timezone).toBe('America/Los_Angeles')
  })

  it('addresses a manual run by zone number', async () => {
    const { calls } = installFetchStub(() => ({ status: 202, body: { accepted: true } }))
    const roses = zoneFixtures[2]!
    expect(roses.id).not.toBe(roses.number)

    await runZone(roses, 600)

    expect(calls[0]?.url).toBe('/api/zones/3/run')
    expect(calls[0]?.method).toBe('POST')
    expect(calls[0]?.body).toEqual({ seconds: 600 })
    expect(calls[0]?.headers['content-type']).toBe('application/json')
  })

  it('addresses a zone update by zone number', async () => {
    const { calls } = installFetchStub()
    const roses = zoneFixtures[2]!

    await putZone(roses, { name: 'Rose bed', enabled: false })

    expect(calls[0]?.url).toBe('/api/zones/3')
    expect(calls[0]?.method).toBe('PUT')
    expect(calls[0]?.body).toEqual({ name: 'Rose bed', enabled: false })
  })

  it('posts a program draft with no ids', async () => {
    const { calls } = installFetchStub(() => ({ status: 201, body: { ok: true } }))

    await createProgram({
      name: 'Evening',
      enabled: true,
      dayMode: 'EveryNDays',
      dowMask: 0,
      intervalDays: 3,
      anchorDate: '2026-04-01',
      startTimes: [{ minutesAfterMidnight: 1140, timezone: 'America/Los_Angeles' }],
      steps: [{ zones: [9], durationSeconds: 300 }],
    })

    expect(calls[0]?.url).toBe('/api/programs')
    expect(calls[0]?.body).toEqual({
      name: 'Evening',
      enabled: true,
      dayMode: 'EveryNDays',
      dowMask: 0,
      intervalDays: 3,
      anchorDate: '2026-04-01',
      startTimes: [{ minutesAfterMidnight: 1140, timezone: 'America/Los_Angeles' }],
      steps: [{ zones: [9], durationSeconds: 300 }],
    })
  })

  it('runs and deletes a program by id', async () => {
    const { calls } = installFetchStub((call) => (call.method === 'DELETE' ? { status: 204 } : { status: 202 }))

    await runProgram(12)
    await deleteProgram(12)

    expect(calls[0]?.url).toBe('/api/programs/12/run')
    expect(calls[0]?.method).toBe('POST')
    expect(calls[1]?.url).toBe('/api/programs/12')
    expect(calls[1]?.method).toBe('DELETE')
  })

  it('stops with no body', async () => {
    const { calls } = installFetchStub(() => ({ status: 202 }))
    await stopAll()
    expect(calls[0]?.url).toBe('/api/stop')
    expect(calls[0]?.method).toBe('POST')
    expect(calls[0]?.body).toBeNull()
  })

  it('stops one zone by zone number with no body', async () => {
    const { calls } = installFetchStub(() => ({ status: 202 }))
    await stopZone(3)
    expect(calls[0]?.url).toBe('/api/zones/3/stop')
    expect(calls[0]?.method).toBe('POST')
    expect(calls[0]?.body).toBeNull()
  })

  it('carries the refusal reason of a 409', async () => {
    installFetchStub(() => ({ status: 409, body: { error: '2 zones already running', reason: 'cap_reached' } }))

    const error = await runZone(zoneFixtures[2]!, 600).catch((caught: unknown) => caught)
    expect(error).toBeInstanceOf(ApiError)
    expect((error as ApiError).status).toBe(409)
    expect((error as ApiError).message).toBe('2 zones already running')
    expect((error as ApiError).reason).toBe('cap_reached')
  })

  it('leaves the reason null when the body has none', async () => {
    installFetchStub(() => ({ status: 404, body: { error: 'unknown zone' } }))
    const error = await runZone(zoneFixtures[2]!, 600).catch((caught: unknown) => caught)
    expect((error as ApiError).reason).toBeNull()
  })

  it('decodes zones through the decoder', async () => {
    installFetchStub(() => ({ body: zoneFixtures }))
    const zones = await getZones()
    expect(zones.map((zone) => zone.number)).toEqual([1, 2, 3, 4, 5, 6, 7, 8])
  })

  it('throws ApiError carrying the status and the server message', async () => {
    installFetchStub(() => ({ status: 404, body: { error: 'unknown zone' } }))

    await expect(runZone(zoneFixtures[2]!, 600)).rejects.toThrowError(ApiError)
    await expect(runZone(zoneFixtures[2]!, 600)).rejects.toThrow(/unknown zone/)

    const error = await runZone(zoneFixtures[2]!, 600).catch((caught: unknown) => caught)
    expect((error as ApiError).status).toBe(404)
  })

  it('throws ApiError when the body is not JSON at all', async () => {
    vi.stubGlobal(
      'fetch',
      vi.fn(async () => new Response('<html>502 Bad Gateway</html>', { status: 502 })),
    )
    await expect(getSettings()).rejects.toThrowError(ApiError)
  })
})
