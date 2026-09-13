import test from 'node:test';
import assert from 'node:assert/strict';
import {generateKeyPairSync} from 'node:crypto';
import {submit} from '../release/chrome-store.mjs';
const {privateKey} = generateKeyPairSync('rsa', {modulusLength:2048});
const item='paachfdeekmbojpfifnaadedhogpgcde';
const base={publisher:'publisher-1',item,version:'0.2.1',archive:Buffer.from('fixture'),credentials:{client_email:'test@example.com',private_key:privateKey.export({type:'pkcs8',format:'pem'})},pause:async()=>{}};
function fixture(responses) { const calls=[]; return {calls,fetcher:async(url, options)=>{calls.push({url,options});const body=responses.shift();return {ok:body.http ? false : true,status:body.http || 200,json:async()=>body};}}; }
test('upload waits for processing before submitting for normal review', async()=>{
 const api=fixture([{access_token:'secret'},{itemId:item,uploadState:'IN_PROGRESS'},{itemId:item,lastAsyncUploadState:'SUCCEEDED'},{itemId:item,state:'PENDING_REVIEW'}]);
 assert.equal((await submit({...base,fetcher:api.fetcher})).state,'PENDING_REVIEW');
 assert.equal(api.calls.length,4);
 assert.equal(api.calls[1].url,`https://chromewebstore.googleapis.com/upload/v2/publishers/publisher-1/items/${item}:upload`);
 assert.deepEqual(JSON.parse(api.calls[3].options.body),{publishType:'DEFAULT_PUBLISH',skipReview:false,blockOnWarnings:true});
});
test('failed upload, wrong item and wrong version never submit for review', async()=>{
 for(const upload of [{itemId:item,uploadState:'FAILED'},{itemId:'other',uploadState:'SUCCEEDED'},{itemId:item,crxVersion:'0.1.0',uploadState:'SUCCEEDED'}]) {
  const api=fixture([{access_token:'secret'},upload]);
  await assert.rejects(submit({...base,fetcher:api.fetcher}));assert.equal(api.calls.length,2);
 }
});
test('authentication errors do not leak credentials or response content', async()=>{
 const api=fixture([{http:403,error:'secret'}]);
 await assert.rejects(submit({...base,fetcher:api.fetcher}), /Google authentication failed \(HTTP 403\)/);
 assert.equal(api.calls.length,1);
});
