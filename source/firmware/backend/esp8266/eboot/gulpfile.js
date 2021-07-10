const path = require('path');
const gulp = require('gulp');
const rename = require('gulp-rename');
const through = require('through2');

const srcFolder = './';
const destFolder = '../source/static/';

const toHeader = function (name, debug) {
	return through.obj((source, encoding, callback) => {
		const parts = source.path.split(path.sep);
		const filename = parts[parts.length - 1];
		const safename = name || filename.split('.').join('_');

		// Generate output
		const buildStamp = new Date();
		let output = '';
		output +=
			'// Build: ' +
			buildStamp +
			'\n' +
			'#include "eboot_bin.h"\n' +
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
		destination.path = source.path + '.c';
		destination.contents = Buffer.from(output);

		if (debug) {
			console.info(
				'Image ' + filename + ' \tsize: ' + source.contents.length + ' bytes'
			);
		}

		callback(null, destination);
	});
};

const ebootToHexArray = function () {
	return gulp
		.src(srcFolder + '*.bin')
		.pipe(rename('eboot_bin'))
		.pipe(gulp.dest(srcFolder))
		.pipe(toHeader('eboot_bin', true))
		.pipe(gulp.dest(destFolder));
};

gulp.task('ebootToHexArray', () => {
	return ebootToHexArray();
});

gulp.task('default', gulp.parallel('ebootToHexArray'));
