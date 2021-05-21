/*

ESP8266 file system builder

Copyright (C) 2016-2019 by Xose Pérez <xose dot perez at gmail dot com>
Adapted in 2021 for esp2ino by Elahd Bar-Shai <elahd at elahd dot com>

This program is free software: you can redistribute it and/or gify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program.  If not, see <http://www.gnu.org/licenses/>.

*/

/* eslint quotes: ['error', 'single'] */
/* eslint-env es6 */

// -----------------------------------------------------------------------------
// Dependencies
// -----------------------------------------------------------------------------
const { series, parallel } = require("gulp");

const path = require("path");

const gulp = require("gulp");
const through = require("through2");

const linthtml = require("@linthtml/gulp-linthtml");
const csslint = require("gulp-csslint");

const htmlmin = require("html-minifier");

const gzip = require("gulp-gzip");
const favicon = require("gulp-base64-favicon");
const inline = require("gulp-inline-source-html");
const rename = require("gulp-rename");
const replace = require("gulp-replace");

// -----------------------------------------------------------------------------
// Configuration
// -----------------------------------------------------------------------------

const htmlFolder = "src/";
const compiledFolder = "compiled/";
const staticFolder = "../backend/static/";

// -----------------------------------------------------------------------------
// Methods
// -----------------------------------------------------------------------------

const toMinifiedHtml = function (options) {
    return through.obj(function (source, encoding, callback) {
        if (source.isNull()) {
            callback(null, source);
            return;
        }

        const contents = source.contents.toString();
        source.contents = Buffer.from(htmlmin.minify(contents, options));
        callback(null, source);
    });
};

const toHeader = function (name, debug) {
    return through.obj(function (source, encoding, callback) {
        const parts = source.path.split(path.sep);
        const filename = parts[parts.length - 1];
        const safename = name || filename.split(".").join("_");

        // Generate output
        let output = "";
        output +=
            "unsigned int " +
            safename +
            "_len = " +
            source.contents.length +
            ";\n";
        output +=
            "const char " + safename + "[] __attribute__((aligned(4))) = {";
        for (let i = 0; i < source.contents.length; i++) {
            if (i > 0) {
                output += ",";
            }
            if (i % 20 === 0) {
                output += "\n";
            }
            output += "0x" + ("00" + source.contents[i].toString(16)).slice(-2);
        }
        output += "\n};";

        // clone the contents
        const destination = source.clone();
        destination.path = source.path + ".h";
        destination.contents = Buffer.from(output);

        if (debug) {
            console.info(
                "Image " +
                    filename +
                    " \tsize: " +
                    source.contents.length +
                    " bytes"
            );
        }

        callback(null, destination);
    });
};

const linthtmlReporter = function (filepath, issues) {
    if (issues.length > 0) {
        issues.forEach(function (issue) {
            console.info(
                "[gulp-eslint] " +
                    filepath +
                    " [" +
                    issue.line +
                    "," +
                    issue.column +
                    "]: " +
                    "(" +
                    issue.code +
                    ") " +
                    issue.msg
            );
        });
        process.exitCode = 1;
    }
};

const inlineHandler = function () {
    return function (source) {
        if (source.sourcepath === "favicon.ico") {
            source.format = "x-icon";
            return;
        }

        if (source.content) {
            return;
        }

        // Just ignore the vendored libs, repackaging makes things worse for the size
        const path = source.sourcepath;
        if (path.endsWith(".min.js")) {
            source.compress = false;
        } else if (path.endsWith(".min.css")) {
            source.compress = false;
        }
    };
};

const buildWebUI = function () {
    return gulp
        .src(htmlFolder + "*.html")
        .pipe(favicon(htmlFolder))
        .pipe(
            linthtml(
                {
                    failOnError: true,
                    configFile: ".linthtmlrc.js",
                },
                linthtmlReporter
            )
        )
        .pipe(inline({ handlers: [inlineHandler()] }))
        .pipe(
            toMinifiedHtml({
                collapseWhitespace: true,
                removeComments: true,
                minifyCSS: false,
                minifyJS: false,
            })
        )
        .pipe(rename("index.html"))
        .pipe(gulp.dest(compiledFolder))
        .pipe(gzip({ gzipOptions: { level: 9 } }))
        .pipe(rename("index.html.gz"))
        .pipe(gulp.dest(compiledFolder))
        .pipe(toHeader("index_html_gz", true))
        .pipe(gulp.dest(staticFolder));
};

// -----------------------------------------------------------------------------
// Tasks
// -----------------------------------------------------------------------------

gulp.task("csslint", function () {
    gulp.src([htmlFolder + "*.css", "!" + htmlFolder + "**.min.css"])
        .pipe(
            csslint({
                ids: false,
            })
        )
        .pipe(csslint.formatter());
});

gulp.task("webui", function () {
    return buildWebUI();
});

// gulp.task('default', gulp.series('csslint', 'webui'))

gulp.task("default", gulp.parallel("webui"));
