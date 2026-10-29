#!/usr/bin/env node
// publishes a release to mainstage (https://mainstage.songspec.org).
//
// the canonical copy lives in songspec/mainstage at tools/mainstage-release.mjs.
// release workflows carry their own copy at .github/scripts/mainstage-release.mjs,
// so update those from the canonical one rather than editing them in place.
//
// needs node 20+ and nothing else. the token comes from the environment:
//
//   MAINSTAGE_RELEASE_TOKEN   a release token for the project (msr_...), required
//   MAINSTAGE_URL             defaults to https://mainstage.songspec.org
//
//   node mainstage-release.mjs \
//     --project centsible --version 1.4.0 --git-ref v1.4.0 \
//     --notes-file docs/release-notes-1.4.0.md \
//     --file dist/Centsible-Setup-1.4.0.exe=windows-installer \
//     --mode live
//
// --file path[=slot]   repeat per file. the slot is the stable link name, so
//                      /dl/<project>/latest/<slot> keeps pointing at that kind
//                      of file. without one, mainstage picks it from earlier
//                      releases of the same kind of file
// --mode live          publish and point every download link at it (default)
// --mode publish       publish to the archive without changing what is live
// --mode draft         leave it as a draft to finish in the mainstage ui
// --existing fail      stop if the version is already published (default)
// --existing fill      add only the files whose slot the published version is
//                      missing, without touching anything already there
// --title, --requirements   optional version metadata
// --dry-run            check the token and print the plan, change nothing
// --preflight          only check the token and that the version can be
//                      released (no --file needed), for failing fast before
//                      a long build
//
// running it again for the same draft version is safe: files with the same
// name or slot are replaced and the notes are updated

import { open, readFile, stat, appendFile } from "node:fs/promises";
import path from "node:path";

const SLOT_RE = /^[a-z0-9]+(?:-[a-z0-9]+)*$/;
const MAX_RETRIES = 6;

class ReleaseError extends Error {}

// thrown rather than exiting on the spot: calling process.exit while fetch
// sockets are still open trips a libuv assertion on windows
function fail(message) {
  throw new ReleaseError(message);
}

function parseArgs(argv) {
  const options = { files: [], mode: "live", existing: "fail", dryRun: false, preflight: false };
  for (let i = 0; i < argv.length; i++) {
    const flag = argv[i];
    const value = () => {
      const next = argv[++i];
      if (next === undefined || next.startsWith("--")) fail(`${flag} needs a value`);
      return next;
    };
    switch (flag) {
      case "--project": options.project = value(); break;
      case "--version": options.version = value(); break;
      case "--git-ref": options.gitRef = value(); break;
      case "--notes-file": options.notesFile = value(); break;
      case "--title": options.title = value(); break;
      case "--requirements": options.requirements = value(); break;
      case "--mode": options.mode = value(); break;
      case "--existing": options.existing = value(); break;
      case "--file": options.files.push(parseFileArg(value())); break;
      case "--dry-run": options.dryRun = true; break;
      case "--preflight": options.preflight = true; break;
      default: fail(`unknown option ${flag}`);
    }
  }
  if (!options.project) fail("--project is required");
  if (!options.version) fail("--version is required");
  if (!["live", "publish", "draft"].includes(options.mode)) fail("--mode must be live, publish or draft");
  if (!["fail", "fill"].includes(options.existing)) fail("--existing must be fail or fill");
  if (options.files.length === 0 && !options.preflight) fail("give at least one --file");
  return options;
}

function parseFileArg(raw) {
  const at = raw.lastIndexOf("=");
  if (at > 0 && SLOT_RE.test(raw.slice(at + 1))) {
    return { path: raw.slice(0, at), slot: raw.slice(at + 1) };
  }
  return { path: raw, slot: undefined };
}

class Mainstage {
  constructor(baseUrl, token) {
    this.baseUrl = baseUrl.replace(/\/+$/, "");
    this.token = token;
  }

  async request(method, apiPath, { json, body, allow = [] } = {}) {
    for (let attempt = 1; ; attempt++) {
      let response;
      try {
        response = await fetch(`${this.baseUrl}/api/release${apiPath}`, {
          method,
          headers: {
            Authorization: `Bearer ${this.token}`,
            ...(json !== undefined ? { "Content-Type": "application/json" } : {}),
            ...(body !== undefined ? { "Content-Type": "application/octet-stream" } : {}),
          },
          body: json !== undefined ? JSON.stringify(json) : body,
        });
      } catch (err) {
        if (attempt >= MAX_RETRIES) throw err;
        await wait(attempt);
        continue;
      }
      if (response.status >= 500 && attempt < MAX_RETRIES) {
        await wait(attempt);
        continue;
      }
      const data = await response.json().catch(() => ({}));
      if (!response.ok && !allow.includes(response.status)) {
        const reason = data.error ?? `${method} ${apiPath} answered ${response.status}`;
        if (response.status === 401) fail(`${reason}. Check the MAINSTAGE_RELEASE_TOKEN secret.`);
        fail(reason);
      }
      return { status: response.status, data };
    }
  }
}

const wait = (attempt) => new Promise((resolve) => setTimeout(resolve, Math.min(15_000, 1_000 * 2 ** (attempt - 1))));

const formatSize = (bytes) => `${(bytes / 1024 / 1024).toFixed(1)} MB`;

async function uploadFile(api, versionId, file) {
  const filename = path.basename(file.path);
  const { size } = await stat(file.path);
  const started = await api.request("POST", `/versions/${versionId}/uploads`, { json: { filename, size } });
  const uploadId = started.data.upload.id;
  const chunkSize = started.data.chunkSize;
  const handle = await open(file.path, "r");

  try {
    let offset = 0;
    let lastReported = -1;
    while (offset < size) {
      const length = Math.min(chunkSize, size - offset);
      const chunk = Buffer.alloc(length);
      await handle.read(chunk, 0, length, offset);
      const result = await api.request("PUT", `/uploads/${uploadId}?offset=${offset}`, { body: chunk, allow: [409] });
      if (result.status === 409) {
        // the server knows best how much arrived, carry on from there
        if (typeof result.data.received !== "number") await wait(2);
        const status = await api.request("GET", `/uploads/${uploadId}`);
        offset = status.data.upload.received;
      } else {
        offset = result.data.received;
      }
      const percent = Math.floor((offset / size) * 100);
      if (percent >= lastReported + 25 || offset === size) {
        console.log(`  ${filename}: ${percent}% of ${formatSize(size)}`);
        lastReported = percent;
      }
    }
  } finally {
    await handle.close();
  }

  const completed = await api.request("POST", `/uploads/${uploadId}/complete`, {
    json: file.slot ? { slot: file.slot } : {},
  });
  return completed.data.asset;
}

async function main() {
  const options = parseArgs(process.argv.slice(2));
  const token = process.env.MAINSTAGE_RELEASE_TOKEN;
  if (!token) fail("MAINSTAGE_RELEASE_TOKEN is not set");
  const baseUrl = process.env.MAINSTAGE_URL || "https://mainstage.songspec.org";
  const api = new Mainstage(baseUrl, token);

  for (const file of options.files) {
    const info = await stat(file.path).catch(() => null);
    if (!info?.isFile()) fail(`${file.path} doesn't exist`);
  }
  let notes;
  if (options.notesFile) {
    notes = await readFile(options.notesFile, "utf8").catch(() => null);
    if (notes === null) fail(`notes file ${options.notesFile} doesn't exist`);
  }

  const project = encodeURIComponent(options.project);
  const versionName = encodeURIComponent(options.version);
  const lookup = await api.request("GET", `/projects/${project}/versions/${versionName}`, { allow: [404] });
  let version = lookup.status === 404 ? null : lookup.data.version;

  if (version?.status === "published" && options.existing === "fail") {
    fail(
      `${options.project} ${options.version} is already published. Bump the version number for a new release, ` +
        `or run again with --existing fill to only add files that version is missing.`,
    );
  }
  if (options.preflight) {
    const state = version ? `the existing ${version.status} version will be ${version.status === "draft" ? "updated" : "filled in"}` : "it is a new version";
    console.log(`mainstage: ${options.project} ${options.version} can be released, ${state}`);
    return;
  }

  const plan = version
    ? version.status === "draft"
      ? `update the existing draft ${options.version}`
      : `add missing files to the published ${options.version}`
    : `create ${options.version} as a draft`;
  console.log(`mainstage: ${options.project} - ${plan}, then ${options.mode === "draft" ? "leave it as a draft" : options.mode === "publish" ? "publish to the archive" : "push it live"}`);
  for (const file of options.files) {
    console.log(`  file ${file.path}${file.slot ? ` as ${file.slot}` : ""}`);
  }
  if (options.dryRun) {
    console.log("dry run: the token works and nothing was changed");
    return;
  }

  const details = {
    ...(notes !== undefined ? { notes } : {}),
    ...(options.title !== undefined ? { title: options.title } : {}),
    ...(options.requirements !== undefined ? { requirements: options.requirements } : {}),
    ...(options.gitRef !== undefined ? { gitRef: options.gitRef } : {}),
  };
  if (!version) {
    const created = await api.request("POST", `/projects/${project}/versions`, {
      json: { version: options.version, ...details },
    });
    version = created.data.version;
  } else if (version.status === "draft" && Object.keys(details).length > 0) {
    const updated = await api.request("PATCH", `/versions/${version.id}`, { json: details });
    version = updated.data.version;
  }

  const uploaded = [];
  for (const file of options.files) {
    const filename = path.basename(file.path);
    const clashes = version.assets.filter(
      (asset) => asset.filename === filename || (file.slot && asset.slot === file.slot),
    );
    if (version.status === "published") {
      if (clashes.length > 0) {
        console.log(`  ${filename}: already there as ${clashes[0].slot}, left alone`);
        continue;
      }
    } else {
      for (const asset of clashes) {
        console.log(`  replacing ${asset.filename} from an earlier run`);
        await api.request("DELETE", `/assets/${asset.id}`);
      }
    }
    const asset = await uploadFile(api, version.id, file);
    uploaded.push(asset);
    console.log(`  ${asset.filename}: stored as ${asset.slot} (sha256 ${asset.sha256})`);
    const refreshed = await api.request("GET", `/versions/${version.id}`);
    version = refreshed.data.version;
  }

  if (options.mode === "live" && !version.isLive) {
    version = (await api.request("POST", `/versions/${version.id}/live`)).data.version;
  } else if (options.mode === "publish" && version.status === "draft") {
    version = (await api.request("POST", `/versions/${version.id}/publish`)).data.version;
  }

  const site = baseUrl.replace(/\/+$/, "");
  const state = version.isLive ? "live" : version.status === "published" ? "published, not live" : "a draft";
  const lines = [
    `${options.project} ${version.version} is ${state} on mainstage.`,
    version.status === "published"
      ? `Page: ${site}/${options.project}/versions/${encodeURIComponent(version.version)}`
      : `Finish it at ${site}/manage/projects/${options.project}/versions/${version.id}`,
    ...version.assets.map((asset) => `- ${asset.label}: ${asset.filename} (${formatSize(asset.size)}) ${site}${asset.stableUrl}`),
  ];
  console.log(`\n${lines.join("\n")}`);
  if (process.env.GITHUB_STEP_SUMMARY) {
    await appendFile(process.env.GITHUB_STEP_SUMMARY, `### Mainstage\n\n${lines.join("\n\n")}\n`);
  }
  if (uploaded.length === 0 && version.status === "published") {
    console.log("nothing new was uploaded");
  }
}

main().catch((err) => {
  console.error(`\nmainstage release failed: ${err instanceof Error ? err.message : String(err)}`);
  process.exitCode = 1;
});
