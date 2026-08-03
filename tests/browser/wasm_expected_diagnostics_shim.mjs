import { EventEmitter } from 'node:events';
import { mkdirSync, writeFileSync } from 'node:fs';
import path from 'node:path';

const expectedDiagnostics = [];
const artifactRoot = process.env.BROWSER_ARTIFACT_DIR
  ? path.resolve(process.env.BROWSER_ARTIFACT_DIR)
  : null;

const redirectPath = '/redirect-gateway/v1/orgs';

function expectedConsoleDiagnostic(message) {
  if (message?.type?.() !== 'error') return null;
  const text = String(message.text?.() ?? '');
  const location = message.location?.() ?? {};
  const url = String(location.url ?? '');

  const exactNetworkErrors = new Set([
    'Failed to load resource: net::ERR_FAILED',
    'Failed to load resource: the server responded with a status of 502 (Bad Gateway)',
    'Failed to load resource: the server responded with a status of 503 (Service Unavailable)',
  ]);
  if (exactNetworkErrors.has(text)) {
    return { kind: 'console', text, url };
  }

  if (
    text.includes('Not allowed to follow a redirection while loading') &&
    text.includes(redirectPath)
  ) {
    return { kind: 'console', text, url };
  }

  return null;
}

function expectedPageError(error) {
  const message = String(error?.message ?? error ?? '');
  if (
    message.includes(redirectPath) &&
    message.includes('due to access control checks')
  ) {
    return { kind: 'pageerror', text: message, url: '' };
  }
  return null;
}

const originalOn = EventEmitter.prototype.on;
EventEmitter.prototype.on = function onWithExpectedBrowserDiagnostics(eventName, listener) {
  if (this?.constructor?.name !== 'Page' || typeof listener !== 'function') {
    return originalOn.call(this, eventName, listener);
  }

  if (eventName === 'console') {
    return originalOn.call(this, eventName, function classifyConsole(message, ...rest) {
      const expected = expectedConsoleDiagnostic(message);
      if (expected) {
        expectedDiagnostics.push(expected);
        return undefined;
      }
      return listener.call(this, message, ...rest);
    });
  }

  if (eventName === 'pageerror') {
    return originalOn.call(this, eventName, function classifyPageError(error, ...rest) {
      const expected = expectedPageError(error);
      if (expected) {
        expectedDiagnostics.push(expected);
        return undefined;
      }
      return listener.call(this, error, ...rest);
    });
  }

  return originalOn.call(this, eventName, listener);
};

process.on('exit', () => {
  if (!artifactRoot) return;
  mkdirSync(artifactRoot, { recursive: true });
  writeFileSync(
    path.join(artifactRoot, 'expected-browser-diagnostics.json'),
    `${JSON.stringify(expectedDiagnostics, null, 2)}\n`,
    { encoding: 'utf8', mode: 0o600 },
  );
});
