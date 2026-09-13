// Chrome Web Store V2. Never log credentials, tokens, or raw auth responses.
import {createSign} from 'node:crypto';
import {readFile} from 'node:fs/promises';
import {pathToFileURL} from 'node:url';
const origin = 'https://chromewebstore.googleapis.com';
export async function submit({publisher, item, version, archive, credentials, fetcher = fetch, pause = ms => new Promise(resolve => setTimeout(resolve, ms))}) {
  if (!/^[A-Za-z0-9_-]+$/.test(publisher || '') || !/^[a-p]{32}$/.test(item || '')) throw Error('Invalid publisher or extension ID');
  if (!/^\d+\.\d+\.\d+$/.test(version || '')) throw Error('Invalid extension version');
  if (!credentials?.client_email || !credentials?.private_key) throw Error('Service-account credentials are incomplete');
  const encode = value => Buffer.from(JSON.stringify(value)).toString('base64url');
  const now = Math.floor(Date.now() / 1000);
  const jwt = `${encode({alg:'RS256',typ:'JWT'})}.${encode({iss:credentials.client_email,scope:'https://www.googleapis.com/auth/chromewebstore',aud:'https://oauth2.googleapis.com/token',iat:now,exp:now+3600})}`;
  const signer = createSign('RSA-SHA256'); signer.update(jwt); signer.end();
  const assertion = `${jwt}.${signer.sign(credentials.private_key, 'base64url')}`;
  const auth = await fetcher('https://oauth2.googleapis.com/token', {method:'POST',redirect:'error',signal:AbortSignal.timeout(60000),body:new URLSearchParams({grant_type:'urn:ietf:params:oauth:grant-type:jwt-bearer',assertion})});
  if (!auth.ok) throw Error(`Google authentication failed (HTTP ${auth.status}); check service-account configuration`);
  const token = (await auth.json()).access_token;
  if (!token) throw Error('Google returned no access token');
  const name = `publishers/${publisher}/items/${item}`;
  const request = async (url, method, body, type) => {
    const response = await fetcher(url, {method,redirect:'error',signal:AbortSignal.timeout(120000),headers:{Authorization:`Bearer ${token}`,...(type ? {'Content-Type':type} : {})},body});
    if (!response.ok) throw Error(`Chrome Web Store ${method} failed (HTTP ${response.status}); inspect the Developer Dashboard before retrying`);
    return response.json();
  };
  const upload = await request(`${origin}/upload/v2/${name}:upload`, 'POST', archive, 'application/zip');
  if (upload.itemId !== item) throw Error('Upload returned a different extension ID');
  if (upload.crxVersion && upload.crxVersion !== version) throw Error('Uploaded version differs from release version');
  let state = upload.uploadState;
  for (let attempt = 0; ['IN_PROGRESS', 'UPLOAD_IN_PROGRESS'].includes(state) && attempt < 30; attempt++) {
    await pause(10000);
    const status = await request(`${origin}/v2/${name}:fetchStatus`, 'GET');
    if (status.itemId !== item) throw Error('Status returned a different extension ID');
    state = status.lastAsyncUploadState;
  }
  if (state !== 'SUCCEEDED') throw Error(`Upload did not succeed (${state || 'unknown'}); nothing was submitted for review`);
  const published = await request(`${origin}/v2/${name}:publish`, 'POST', JSON.stringify({publishType:'DEFAULT_PUBLISH',skipReview:false,blockOnWarnings:true}), 'application/json');
  if (published.itemId !== item || !published.state) throw Error('Unrecognized submission response; check the Developer Dashboard before retrying');
  return {itemId:item,version,state:published.state};
}
if (process.argv[1] && import.meta.url === pathToFileURL(process.argv[1]).href) {
  try {
    const metadata = JSON.parse(await readFile('package.json', 'utf8'));
    const store = JSON.parse(await readFile('chrome-store.json', 'utf8'));
    const result = await submit({publisher:process.env.CWS_PUBLISHER_ID,item:store.itemId,version:metadata.version,archive:await readFile(process.argv[2]),credentials:JSON.parse(process.env.CWS_SERVICE_ACCOUNT_JSON || '{}')});
    console.log(JSON.stringify(result));
    console.log('Submission sent to Google. Store review/approval and rollout are separate.');
  } catch (error) {
    // JSON/crypto exceptions can contain fragments of the input: print only our own safe errors.
    console.error(error instanceof SyntaxError || error.code ? 'Unable to read release inputs or service-account key; check configuration.' : error.message);
    process.exitCode = 1;
  }
}
