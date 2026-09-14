// @vitest-environment node
import { spawnSync } from 'node:child_process'
import { copyFileSync, mkdtempSync, mkdirSync, rmSync, writeFileSync } from 'node:fs'
import { tmpdir } from 'node:os'
import { join } from 'node:path'
import { fileURLToPath } from 'node:url'
import { afterEach, describe, expect, it } from 'vitest'
import { checkBundleDir, scanForExternalOrigins } from './checkBundle.mjs'

const temporaryDirs = []

afterEach(() => {
  for (const dir of temporaryDirs.splice(0)) {
    rmSync(dir, { recursive: true, force: true })
  }
})

function bundle(files) {
  const dir = mkdtempSync(join(tmpdir(), 'irrigation-bundle-'))
  temporaryDirs.push(dir)
  for (const [name, content] of Object.entries(files)) {
    const path = join(dir, name)
    mkdirSync(join(path, '..'), { recursive: true })
    writeFileSync(path, content)
  }
  return dir
}

describe('scanForExternalOrigins', () => {
  it('finds a stylesheet on a remote host', () => {
    const found = scanForExternalOrigins(
      '<link rel="stylesheet" href="https://fonts.googleapis.com/css2?family=Inter">',
    )
    expect(found).toContain('https://fonts.googleapis.com')
  })

  it('finds a script on a remote host', () => {
    expect(scanForExternalOrigins('<script src="http://cdn.example.com/react.js"></script>')).toContain(
      'http://cdn.example.com',
    )
  })

  it('finds a protocol-relative url', () => {
    expect(scanForExternalOrigins('<img src="//images.example.com/logo.png">')).toContain(
      '//images.example.com',
    )
  })

  it('finds a remote origin inside bundled javascript', () => {
    expect(scanForExternalOrigins('fetch("https://api.weather.example.com/v1")')).toContain(
      'https://api.weather.example.com',
    )
  })

  it('finds a host given as an ip address, with or without a port', () => {
    expect(scanForExternalOrigins('fetch("http://203.0.113.5/api")')).toContain('http://203.0.113.5')
    expect(scanForExternalOrigins('fetch("http://192.168.1.50:8080/api")')).toContain('http://192.168.1.50:8080')
  })

  it('finds a development-machine url on localhost', () => {
    expect(scanForExternalOrigins('fetch("http://localhost:8080/admin/status")')).toContain('http://localhost:8080')
  })

  it('finds a websocket origin', () => {
    expect(scanForExternalOrigins('new WebSocket("ws://192.168.1.50:9000/socket")')).toContain('ws://192.168.1.50:9000')
  })

  it('ignores a protocol-relative double slash with no dotted host after it', () => {
    expect(scanForExternalOrigins('const separator = "//section"')).toEqual([])
  })

  it('matches the allowlist regardless of letter case', () => {
    expect(scanForExternalOrigins('<svg xmlns="HTTP://WWW.W3.ORG/2000/svg"></svg>')).toEqual([])
  })

  it('allows the XML namespace urls that svg markup carries', () => {
    expect(
      scanForExternalOrigins('<svg xmlns="http://www.w3.org/2000/svg"><path d="M0 0"/></svg>'),
    ).toEqual([])
    expect(scanForExternalOrigins('<html xmlns="http://www.w3.org/1999/xhtml">')).toEqual([])
  })

  it('allows the error-reference links React embeds in its production build', () => {
    expect(scanForExternalOrigins('Error("Minified React error #"+e+"; visit https://react.dev/errors/"+e)')).toEqual([])
  })

  it('reports any other path on a host with an allowed prefix', () => {
    expect(scanForExternalOrigins('fetch("https://react.dev/api/telemetry")')).toContain('https://react.dev')
  })

  it('reports a host whose name only begins with an allowed one', () => {
    expect(scanForExternalOrigins('<script src="http://www.w3.org.example.com/x.js"></script>')).toContain(
      'http://www.w3.org.example.com',
    )
  })

  it('allows root-relative and same-origin urls', () => {
    expect(
      scanForExternalOrigins('<script type="module" src="/assets/index-abc123.js"></script>'),
    ).toEqual([])
    expect(scanForExternalOrigins('fetch("/api/status")')).toEqual([])
  })
})

describe('checkBundleDir', () => {
  it('passes a clean bundle', () => {
    const dir = bundle({
      'index.html': '<!doctype html><script type="module" src="/assets/app.js"></script>',
      'assets/app.js': 'fetch("/api/status")',
      'assets/app.css': 'body { font-family: system-ui; }',
    })
    expect(checkBundleDir(dir)).toEqual([])
  })

  it('reports a missing directory', () => {
    expect(checkBundleDir(join(tmpdir(), 'irrigation-bundle-does-not-exist'))).toEqual([
      expect.stringMatching(/does not exist/i),
    ])
  })

  it('reports a directory with no index.html', () => {
    const dir = bundle({ 'assets/app.js': 'fetch("/api/status")' })
    expect(checkBundleDir(dir)).toEqual([expect.stringMatching(/index\.html/i)])
  })

  it('reports an external origin in the entry document', () => {
    const dir = bundle({
      'index.html': '<link href="https://fonts.googleapis.com/css2?family=Inter" rel="stylesheet">',
    })
    expect(checkBundleDir(dir).join('\n')).toMatch(/fonts\.googleapis\.com/)
  })

  it('reports an external origin in an emitted asset', () => {
    const dir = bundle({
      'index.html': '<!doctype html><script src="/assets/app.js"></script>',
      'assets/app.js': 'new Image().src = "https://tracker.example.com/pixel.gif"',
    })
    expect(checkBundleDir(dir).join('\n')).toMatch(/tracker\.example\.com/)
  })

  it('ignores source maps', () => {
    const dir = bundle({
      'index.html': '<!doctype html><script src="/assets/app.js"></script>',
      'assets/app.js': '//# sourceMappingURL=app.js.map',
      'assets/app.js.map': '{"sources":["https://internal.example.com/src/app.ts"]}',
    })
    expect(checkBundleDir(dir)).toEqual([])
  })
})

describe('the build-step command', () => {
  function copyCommand() {
    const dir = mkdtempSync(join(tmpdir(), 'irrigation bundle command '))
    temporaryDirs.push(dir)
    for (const name of ['checkBundle.mjs', 'checkBundleCli.mjs']) {
      copyFileSync(fileURLToPath(new URL(name, import.meta.url)), join(dir, name))
    }
    return join(dir, 'checkBundleCli.mjs')
  }

  it('exits non-zero naming the origin, even from a path containing spaces', () => {
    const dir = bundle({ 'index.html': '<link href="https://fonts.googleapis.com/css2" rel="stylesheet">' })
    const result = spawnSync(process.execPath, [copyCommand(), dir], { encoding: 'utf8' })
    expect(result.status).toBe(1)
    expect(result.stderr).toMatch(/fonts\.googleapis\.com/)
  })

  it('exits zero on a clean bundle', () => {
    const dir = bundle({ 'index.html': '<!doctype html><script type="module" src="/assets/app.js"></script>' })
    const result = spawnSync(process.execPath, [copyCommand(), dir], { encoding: 'utf8' })
    expect(result.status).toBe(0)
  })
})
