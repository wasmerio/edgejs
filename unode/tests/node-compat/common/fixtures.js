'use strict';

const path = require('path');
const fs = require('fs');
const { pathToFileURL } = require('url');

// When running raw Node tests (NODE_TEST_DIR set), use node/test/fixtures so paths match Node.
const fixturesDir = typeof process !== 'undefined' && process.env && process.env.NODE_TEST_DIR
  ? path.join(process.env.NODE_TEST_DIR, 'fixtures')
  : path.join(__dirname, '..', 'fixtures');

function fixturesPath(...args) {
  return path.join(fixturesDir, ...args);
}

function fixturesFileURL(...args) {
  return pathToFileURL(fixturesPath(...args));
}

function readFixtureSync(args, enc) {
  const p = Array.isArray(args) ? fixturesPath(...args) : fixturesPath(args);
  if (enc == null) return fs.readFileSync(p);
  return Buffer.from(fs.readFileSync(p)).toString(enc);
}

function readKey(...args) {
  let encoding;
  if (args.length > 1 && typeof args[args.length - 1] === 'string') {
    encoding = args.pop();
  }
  const keyPathParts = args[0] && String(args[0]).includes('/') ? args : ['keys', ...args];
  const b = fs.readFileSync(fixturesPath(...keyPathParts));
  return encoding ? Buffer.from(b).toString(encoding) : b;
}

// Same string as Node test/fixtures/utf8_test_text.txt (used by test-fs-append-file-sync etc.)
const utf8TestText = '永和九年，嵗在癸丑，暮春之初，會於會稽山隂之蘭亭，脩稧事也。' +
  '羣賢畢至，少長咸集。此地有崇山峻領，茂林脩竹；又有清流激湍，' +
  '暎帶左右。引以為流觴曲水，列坐其次。雖無絲竹管弦之盛，一觴一詠，' +
  '亦足以暢敘幽情。是日也，天朗氣清，恵風和暢；仰觀宇宙之大，' +
  '俯察品類之盛；所以遊目騁懐，足以極視聽之娛，信可樂也。夫人之相與，' +
  '俯仰一世，或取諸懐抱，悟言一室之內，或因寄所託，放浪形骸之外。' +
  '雖趣舎萬殊，靜躁不同，當其欣扵所遇，暫得扵己，怏然自足，' +
  '不知老之將至。及其所之既惓，情隨事遷，感慨係之矣。向之所欣，' +
  '俛仰之閒以為陳跡，猶不能不以之興懐；況脩短隨化，終期扵盡。' +
  '古人云：「死生亦大矣。」豈不痛哉！每攬昔人興感之由，若合一契，' +
  '未嘗不臨文嗟悼，不能喻之扵懐。固知一死生為虛誕，齊彭殤為妄作。' +
  '後之視今，亦由今之視昔，悲夫！故列敘時人，錄其所述，雖世殊事異，' +
  '所以興懐，其致一也。後之攬者，亦將有感扵斯文。';

module.exports = {
  fixturesDir,
  path: fixturesPath,
  fileURL: fixturesFileURL,
  readSync: readFixtureSync,
  readKey,
  utf8TestText,
  get utf8TestTextPath() {
    return fixturesPath('utf8_test_text.txt');
  },
};
