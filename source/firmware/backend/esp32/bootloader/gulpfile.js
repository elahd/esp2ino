// -----------------------------------------------------------------------------
// Dependencies
// -----------------------------------------------------------------------------
const { series, parallel } = require('gulp');

const path = require('path');
const gulp = require('gulp');
const through = require('through2');
const rename = require('gulp-rename');
// const replace = require('gulp-replace');

// -----------------------------------------------------------------------------
// Configuration
// -----------------------------------------------------------------------------

const binFolder = './';
const staticFolder = '../../../source/firmware/backend/shared/static/';

// -----------------------------------------------------------------------------
// Methods
// -----------------------------------------------------------------------------

const toHeader = function (name, debug) {
	return through.obj(function (source, encoding, callback) {
		const parts = source.path.split(path.sep);
		const filename = parts[parts.length - 1];
		const safename = name || filename.split('.').join('_');

		// Generate output
		let buildStamp = new Date();
		let output = '';
		output +=
			'// Build: ' +
			buildStamp +
			'\n' +
			'unsigned int ' +
			safename +
			'_len = ' +
			source.contents.length +
			';\n';
		output +=
			'unsigned char ' + safename + '[] __attribute__((aligned(4))) = {';
		for (let i = 0; i < source.contents.length; i++) {
			if (i > 0) {
				output += ',';
			}
			if (i % 20 === 0) {
				output += '\n';
			}
			output += '0x' + ('00' + source.contents[i].toString(16)).slice(-2);
		}
		output += '\n};';

		// clone the contents
		const destination = source.clone();
		destination.path = source.path + '.h';
		destination.contents = Buffer.from(output);

		if (debug) {
			console.info(
				'Image ' + filename + ' \tsize: ' + source.contents.length + ' bytes'
			);
		}

		callback(null, destination);
	});
};

const buildBootloader = function () {
	return gulp
		.src(binFolder + '*.bin')
		.pipe(rename('eboot_esp32_bin'))
		.pipe(toHeader('eboot_bin', true))
		.pipe(gulp.dest(staticFolder));
};

// -----------------------------------------------------------------------------
// Tasks
// -----------------------------------------------------------------------------

gulp.task('buildBootloader', function () {
	return buildBootloader();
});

gulp.task('default', gulp.task('buildBootloader'));
