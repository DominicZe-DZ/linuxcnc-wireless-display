// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Raúl Milla (DominicZe)
//
// Rasterises the SVG jobs sent on stdin to PNG with sharp. Only used when
// regenerating assets; building the firmware needs neither Node nor sharp.
const sharp = require('sharp');
let input='';process.stdin.setEncoding('utf8');process.stdin.on('data',s=>input+=s);
process.stdin.on('end',async()=>{
 try{
  const result=[];
  for(const job of JSON.parse(input)){
   const png=await sharp(Buffer.from(job.svg)).resize(job.width,job.height).png().toBuffer();
   result.push({name:job.name,png:png.toString('base64')});
  }
  process.stdout.write(JSON.stringify(result));
 }catch(e){process.stderr.write(e.message);process.exitCode=1;}
});
