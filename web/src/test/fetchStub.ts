import { vi } from 'vitest'

export interface RecordedCall {
  url: string
  method: string
  body: unknown
  headers: Record<string, string>
}

export interface StubbedResponse {
  status?: number
  body?: unknown
}

/**
 * Replaces globalThis.fetch. `respond` is consulted per call; returning undefined
 * yields 200 with an empty object.
 */
export function installFetchStub(
  respond: (call: RecordedCall) => StubbedResponse | undefined = () => undefined,
) {
  const calls: RecordedCall[] = []

  const stub = vi.fn(async (input: RequestInfo | URL, init?: RequestInit): Promise<Response> => {
    const rawBody = init?.body
    const call: RecordedCall = {
      url: String(input),
      method: init?.method ?? 'GET',
      body: typeof rawBody === 'string' && rawBody.length > 0 ? JSON.parse(rawBody) : null,
      headers: { ...((init?.headers as Record<string, string>) ?? {}) },
    }
    calls.push(call)

    const reply = respond(call) ?? {}
    const status = reply.status ?? 200
    const payload = reply.body === undefined ? {} : reply.body

    return new Response(status === 204 ? null : JSON.stringify(payload), {
      status,
      headers: { 'content-type': 'application/json' },
    })
  })

  vi.stubGlobal('fetch', stub)
  return { calls, stub }
}
