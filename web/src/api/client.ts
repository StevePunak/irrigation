import { apiUrl } from './apiPath'
import { decodePrograms, decodeSettings, decodeStatus, decodeZones } from './decode'
import {
  ApiError,
  type Program,
  type ProgramDraft,
  type SettingsMap,
  type Status,
  type Zone,
} from './types'

interface RequestOptions {
  method?: string
  body?: unknown
}

async function request(path: string, options: RequestOptions = {}): Promise<unknown> {
  const method = options.method ?? 'GET'
  const init: RequestInit = { method }

  if (options.body !== undefined) {
    init.headers = { 'content-type': 'application/json' }
    init.body = JSON.stringify(options.body)
  }

  const response = await fetch(apiUrl(path), init)

  if (response.status === 204) {
    return null
  }

  const text = await response.text()
  let payload: unknown = null

  if (text.length > 0) {
    try {
      payload = JSON.parse(text)
    } catch {
      if (response.ok === false) {
        throw new ApiError(response.status, `${response.status} ${response.statusText}`)
      }
      throw new ApiError(response.status, 'response body was not JSON')
    }
  }

  if (response.ok === false) {
    const detail =
      typeof payload === 'object' && payload !== null && typeof (payload as Record<string, unknown>)['error'] === 'string'
        ? ((payload as Record<string, unknown>)['error'] as string)
        : `${response.status} ${response.statusText}`
    throw new ApiError(response.status, detail)
  }

  return payload
}

export async function getStatus(): Promise<Status> {
  return decodeStatus(await request('/status'))
}

export async function getZones(): Promise<Zone[]> {
  return decodeZones(await request('/zones'))
}

export async function putZone(zone: Zone, patch: { name: string; enabled: boolean }): Promise<void> {
  await request(`/zones/${zone.number}`, { method: 'PUT', body: patch })
}

export async function runZone(zone: Zone, seconds: number): Promise<void> {
  await request(`/zones/${zone.number}/run`, { method: 'POST', body: { seconds } })
}

export async function getPrograms(): Promise<Program[]> {
  return decodePrograms(await request('/programs'))
}

export async function createProgram(draft: ProgramDraft): Promise<void> {
  await request('/programs', { method: 'POST', body: draft })
}

export async function updateProgram(id: number, draft: ProgramDraft): Promise<void> {
  await request(`/programs/${id}`, { method: 'PUT', body: draft })
}

export async function deleteProgram(id: number): Promise<void> {
  await request(`/programs/${id}`, { method: 'DELETE' })
}

export async function runProgram(id: number): Promise<void> {
  await request(`/programs/${id}/run`, { method: 'POST' })
}

export async function stopAll(): Promise<void> {
  await request('/stop', { method: 'POST' })
}

export async function getSettings(): Promise<SettingsMap> {
  return decodeSettings(await request('/settings'))
}

export async function putSettings(patch: SettingsMap): Promise<void> {
  await request('/settings', { method: 'PUT', body: patch })
}
