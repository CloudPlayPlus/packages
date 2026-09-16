group = "dev.flutter.packages.file_selector_android"
version = "1.0"

buildscript {
    val kotlinVersion = "2.3.0"
    repositories {
        google()
        mavenCentral()
    }

    dependencies {
        classpath("com.android.tools.build:gradle:8.13.1")
        classpath("org.jetbrains.kotlin:kotlin-gradle-plugin:$kotlinVersion")
    }
}

allprojects {
    repositories {
        google()
        mavenCentral()
    }
}

plugins {
    id("com.android.library")
}

// AGP 9 provides Kotlin unless the app opts out. Older Flutter/AGP consumers
// still need KGP, but applying it with built-in Kotlin enabled causes a conflict.
val usesBuiltInKotlin =
    com.android.Version.ANDROID_GRADLE_PLUGIN_VERSION.substringBefore('.').toInt() >= 9 &&
        providers.gradleProperty("android.builtInKotlin").orNull != "false"
if (!usesBuiltInKotlin) {
    apply(plugin = "kotlin-android")
    tasks.withType<org.jetbrains.kotlin.gradle.tasks.KotlinCompile>().configureEach {
        compilerOptions {
            jvmTarget = org.jetbrains.kotlin.gradle.dsl.JvmTarget.JVM_17
        }
    }
}

android {
    namespace = "dev.flutter.packages.file_selector_android"
    compileSdk = flutter.compileSdkVersion

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }

    defaultConfig {
        minSdk = 24
    }

    dependencies {
        implementation("androidx.annotation:annotation:1.10.0")
        testImplementation("junit:junit:4.13.2")
        testImplementation("org.mockito:mockito-core:5.23.0")
        testImplementation("androidx.test:core:1.7.0")
        testImplementation("org.robolectric:robolectric:4.16")
    }

    lint {
        checkAllWarnings = true
        warningsAsErrors = true
        disable.addAll(setOf("AndroidGradlePluginVersion", "InvalidPackage", "GradleDependency", "NewerVersionAvailable"))
    }

    testOptions {
        unitTests {
            isIncludeAndroidResources = true
            isReturnDefaultValues = true
            all {
                it.outputs.upToDateWhen { false }
                it.testLogging {
                    events("passed", "skipped", "failed", "standardOut", "standardError")
                    showStandardStreams = true
                }
            }
        }
    }
}
