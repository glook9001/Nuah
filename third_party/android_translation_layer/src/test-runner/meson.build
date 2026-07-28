srcs = [
# test-runner
	'android/test/ActivityInstrumentationTestCase2.java',
	'android/test/ActivityInstrumentationTestCase.java',
	'android/test/ActivityTestCase.java',
	'android/test/ActivityUnitTestCase.java',
	'android/test/AndroidTestRunner.java',
	'android/test/ApplicationTestCase.java',
	'android/test/AssertionFailedError.java',
	'android/test/ClassPathPackageInfoSource.java',
	'android/test/ComparisonFailure.java',
	'android/test/InstrumentationTestRunner.java',
#	'android/test/IsolatedContext.java',
	'android/test/LaunchPerformanceBase.java',
#	'android/test/LoaderTestCase.java',
	'android/test/MoreAsserts.java',
	'android/test/NoExecTestResult.java',
#	'android/test/ProviderTestCase2.java',
#	'android/test/ProviderTestCase.java',
	'android/test/RenamingDelegatingContext.java',
#	'android/test/ServiceTestCase.java',
	'android/test/SimpleCache.java',
#	'android/test/SingleLaunchActivityTestCase.java',
	'android/test/SyncBaseInstrumentation.java',
	'android/test/TestCaseUtil.java',
	'android/test/TestPrinter.java',
	'android/test/TestSuiteProvider.java',
#	'android/test/TouchUtils.java',
	'android/test/ViewAsserts.java',
	'android/test/suitebuilder/AssignableFrom.java',
	'android/test/suitebuilder/SmokeTestSuiteBuilder.java',
	'android/test/suitebuilder/TestGrouping.java',
	'android/test/suitebuilder/TestMethod.java',
	'android/test/suitebuilder/TestPredicates.java',
	'android/test/suitebuilder/TestSuiteBuilder.java',
	'android/test/suitebuilder/UnitTestSuiteBuilder.java',
# old stuff (needed for older CTS versions)
	'android/test/suitebuilder/annotation/HasAnnotation.java',
	'android/test/suitebuilder/annotation/HasClassAnnotation.java',
	'android/test/suitebuilder/annotation/HasMethodAnnotation.java',
	'com/android/internal/util/Predicates.java',
# test-base
	'android/test/AndroidTestCase.java',
	'android/test/FlakyTest.java',
	'android/test/InstrumentationTestCase.java',
	'android/test/InstrumentationTestSuite.java',
	'android/test/PerformanceTestCase.java',
	'android/test/RepetitiveTest.java',
	'android/test/UiThreadTest.java',
	'android/test/suitebuilder/annotation/LargeTest.java',
	'android/test/suitebuilder/annotation/MediumTest.java',
	'android/test/suitebuilder/annotation/SmallTest.java',
	'android/test/suitebuilder/annotation/Smoke.java',
	'android/test/suitebuilder/annotation/Suppress.java',
	'com/android/internal/util/Predicate.java',
# this was part of dalvik but got removed at some point
	'dalvik/annotation/BrokenTest.java',
	'dalvik/annotation/SideEffect.java',
]

java_args = [
	'-bootclasspath', bootclasspath,
	'-source', '1.8', '-target', '1.8',
	'-encoding', 'UTF-8',
	'-Xlint:-deprecation', # we implement deprecated APIs
	'-h', join_paths(dir_base, 'src/api-impl-jni/generated_headers')
]

ant = find_program('ant', required: false)

if ant.found()
	test_runner_jar = custom_target('test_runner',
	                        output: 'test_runner.jar',
	                        command: [ant,
	                                 '-buildfile', files('build.xml'),
	                                 '-Dbasedir=' + builddir_base,
	                                 '-Dsrcdir=' + meson.current_source_dir(),
	                                 '-Dincludes=' + ','.join(srcs),
	                                 '-Djarfile=@OUTPUT@',
	                                 '-Dbuild=@PRIVATE_DIR@',
	                                 '-Djavaargs=' + ' '.join(java_args + ['-classpath', hax_jar.full_path()]),
	                        ],
	                        depend_files: srcs + ['build.xml', hax_jar.full_path()],
	)
else
	test_runner_jar = jar('test_runner', srcs,
	                          build_by_default: false,
	                          link_with: [hax_jar],
	                          java_args: java_args,)
endif
