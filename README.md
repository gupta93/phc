# PHC

phc is an open source compiler for PHP with support for plugins. In addition, it can be used to pretty-print or obfuscate PHP code, as a framework for developing applications that process PHP scripts, or to convert PHP into XML and back, enabling processing of PHP scripts using XML tools.

See http://phpcompiler.org for more details.

## Building from source

After checking out via git, run

	$ touch src/generated/*
	$ ./configure
	$ make
	$ make install

For more detail, see the manual, in docs/manual/.


## Testing

The tests are held in test/framework/, but tests should be run from this
directory.  test/support_files/ are files used by the tests, for example for
regression.  test/subjects/ are the files used to test phc. Most are designed
to test some feature or tickle some bug. test/subjects/labels controls what
tests are run on which subjects. test/logs/ contains logs of failed tests.
test/dependencies are used to avoid running a test if we know it wont work
because a previous test failed. test/working contains the working directory for
each test run.

To run tests:

	$ make test # aka make check

or

	$ make installcheck

or

	$ make long-test

or

	$ php test/framework/driver.php

The latter allows command line options, and limiting the tests with regular
expressions. Run driver.php with the '-h' flag for details.

In order to run tests on your own php files, add the files to
test/subjects/3rdparty/, and list them in test/subjects/3rdparty/labels. In order to
generate the support files, for example if you'd like the regression tests to
succeed rather than be skipped, run

	$ make generate-test-files

By default, 3rdparty tests are treated as 'long', so need to be run with

	$ make long-test


Its also straight-forward to test PHP's phpt test files:
	- run the test suite on PHP (in PHP's directory). This creates .php files from the .phpt files
		$ TEST_PHP_EXECUTABLE=./sapi/cli/php --keep=php

	- link the PHP phpt directory from test/subjects/3rdparty.
	- update test/subjects/3rdparty/labels
	- Run the tests using the .php files.
	- This doesnt provide the EXPECT functionality etc, but we dont really need that in the majority of cases.
