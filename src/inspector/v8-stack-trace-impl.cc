// Copyright 2016 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/inspector/v8-stack-trace-impl.h"

#include "src/inspector/string-util.h"
#include "src/inspector/v8-debugger-agent-impl.h"
#include "src/inspector/v8-debugger.h"
#include "src/inspector/v8-inspector-impl.h"

#include "include/v8-version.h"

namespace v8_inspector {

namespace {

static const v8::StackTrace::StackTraceOptions stackTraceOptions =
    static_cast<v8::StackTrace::StackTraceOptions>(
        v8::StackTrace::kLineNumber | v8::StackTrace::kColumnOffset |
        v8::StackTrace::kScriptId | v8::StackTrace::kScriptNameOrSourceURL |
        v8::StackTrace::kFunctionName |
        v8::StackTrace::kExposeFramesAcrossSecurityOrigins);

V8StackTraceImpl::Frame toFrame(v8::Local<v8::StackFrame> frame,
                                WasmTranslation* wasmTranslation) {
  String16 scriptId = String16::fromInteger(frame->GetScriptId());
  String16 sourceName;
  v8::Local<v8::String> sourceNameValue(frame->GetScriptNameOrSourceURL());
  if (!sourceNameValue.IsEmpty())
    sourceName = toProtocolString(sourceNameValue);

  String16 functionName;
  v8::Local<v8::String> functionNameValue(frame->GetFunctionName());
  if (!functionNameValue.IsEmpty())
    functionName = toProtocolString(functionNameValue);

  int sourceLineNumber = frame->GetLineNumber() - 1;
  int sourceColumn = frame->GetColumn() - 1;
  // TODO(clemensh): Figure out a way to do this translation only right before
  // sending the stack trace over wire.
  if (frame->IsWasm()) {
    wasmTranslation->TranslateWasmScriptLocationToProtocolLocation(
        &scriptId, &sourceLineNumber, &sourceColumn);
  }
  return V8StackTraceImpl::Frame(functionName, scriptId, sourceName,
                                 sourceLineNumber + 1, sourceColumn + 1);
}

void toFramesVector(v8::Local<v8::StackTrace> stackTrace,
                    std::vector<V8StackTraceImpl::Frame>& frames,
                    size_t maxStackSize, v8::Isolate* isolate,
                    V8Debugger* debugger) {
  DCHECK(isolate->InContext());
  int frameCount = stackTrace->GetFrameCount();
  if (frameCount > static_cast<int>(maxStackSize))
    frameCount = static_cast<int>(maxStackSize);
  WasmTranslation* wasmTranslation = debugger->wasmTranslation();
  for (int i = 0; i < frameCount; i++) {
    v8::Local<v8::StackFrame> stackFrame = stackTrace->GetFrame(i);
    frames.push_back(toFrame(stackFrame, wasmTranslation));
  }
}

}  //  namespace

V8StackTraceImpl::Frame::Frame(const String16& functionName,
                               const String16& scriptId,
                               const String16& scriptName, int lineNumber,
                               int column)
    : m_functionName(functionName),
      m_scriptId(scriptId),
      m_scriptName(scriptName),
      m_lineNumber(lineNumber),
      m_columnNumber(column) {
  DCHECK(m_lineNumber != v8::Message::kNoLineNumberInfo);
  DCHECK(m_columnNumber != v8::Message::kNoColumnInfo);
}

// buildInspectorObject() and SourceLocation's toTracedValue() should set the
// same fields.
// If either of them is modified, the other should be also modified.
std::unique_ptr<protocol::Runtime::CallFrame>
V8StackTraceImpl::Frame::buildInspectorObject() const {
  return protocol::Runtime::CallFrame::create()
      .setFunctionName(m_functionName)
      .setScriptId(m_scriptId)
      .setUrl(m_scriptName)
      .setLineNumber(m_lineNumber - 1)
      .setColumnNumber(m_columnNumber - 1)
      .build();
}

// static
void V8StackTraceImpl::setCaptureStackTraceForUncaughtExceptions(
    v8::Isolate* isolate, bool capture) {
  isolate->SetCaptureStackTraceForUncaughtExceptions(
      capture, V8StackTraceImpl::maxCallStackSizeToCapture, stackTraceOptions);
}

// static
std::unique_ptr<V8StackTraceImpl> V8StackTraceImpl::create(
    V8Debugger* debugger, int contextGroupId,
    v8::Local<v8::StackTrace> stackTrace, size_t maxStackSize,
    const String16& description) {
  DCHECK(debugger);
  v8::Isolate* isolate = debugger->inspector()->isolate();
  v8::HandleScope scope(isolate);
  std::vector<V8StackTraceImpl::Frame> frames;
  if (!stackTrace.IsEmpty()) {
    toFramesVector(stackTrace, frames, maxStackSize, isolate, debugger);
  }

  int maxAsyncCallChainDepth = 1;
  V8StackTraceImpl* asyncCallChain = nullptr;
  V8StackTraceImpl* creationStack = nullptr;
  if (maxStackSize > 1) {
    asyncCallChain = debugger->currentAsyncCallChain();
    creationStack = debugger->currentAsyncTaskCreationStack();
    maxAsyncCallChainDepth = debugger->maxAsyncCallChainDepth();
  }

  // Do not accidentally append async call chain from another group. This should
  // not happen if we have proper instrumentation, but let's double-check to be
  // safe.
  if (contextGroupId && asyncCallChain && asyncCallChain->m_contextGroupId &&
      asyncCallChain->m_contextGroupId != contextGroupId) {
    asyncCallChain = nullptr;
    creationStack = nullptr;
    maxAsyncCallChainDepth = 1;
  }

  // Only the top stack in the chain may be empty and doesn't contain creation
  // stack , so ensure that second stack is non-empty (it's the top of appended
  // chain).
  if (asyncCallChain && !creationStack && !asyncCallChain->m_creation &&
      asyncCallChain->isEmpty()) {
    asyncCallChain = asyncCallChain->m_parent.get();
  }

  if (frames.empty() && !creationStack && !asyncCallChain) return nullptr;

  // When async call chain is empty but doesn't contain useful schedule stack
  // and parent async call chain contains creationg stack but doesn't
  // synchronous we can merge them together.
  // e.g. Promise ThenableJob.
  if (asyncCallChain && frames.empty() &&
      asyncCallChain->m_description == description && !creationStack) {
    return asyncCallChain->cloneImpl();
  }

  std::unique_ptr<V8StackTraceImpl> result(new V8StackTraceImpl(
      contextGroupId, description, frames,
      asyncCallChain ? asyncCallChain->cloneImpl() : nullptr,
      creationStack ? creationStack->cloneImpl() : nullptr));

  // Crop to not exceed maxAsyncCallChainDepth.
  V8StackTraceImpl* deepest = result.get();
  while (deepest && maxAsyncCallChainDepth) {
    deepest = deepest->m_parent.get();
    maxAsyncCallChainDepth--;
  }
  if (deepest) deepest->m_parent.reset();

  return result;
}

// static
std::unique_ptr<V8StackTraceImpl> V8StackTraceImpl::capture(
    V8Debugger* debugger, int contextGroupId, size_t maxStackSize,
    const String16& description) {
  DCHECK(debugger);
  v8::Isolate* isolate = debugger->inspector()->isolate();
  v8::HandleScope handleScope(isolate);
  v8::Local<v8::StackTrace> stackTrace;
  if (isolate->InContext()) {
    stackTrace = v8::StackTrace::CurrentStackTrace(
        isolate, static_cast<int>(maxStackSize), stackTraceOptions);
  }
  return V8StackTraceImpl::create(debugger, contextGroupId, stackTrace,
                                  maxStackSize, description);
}

std::unique_ptr<V8StackTraceImpl> V8StackTraceImpl::cloneImpl() {
  std::vector<Frame> framesCopy(m_frames);
  std::unique_ptr<V8StackTraceImpl> copy(
      new V8StackTraceImpl(m_contextGroupId, m_description, framesCopy,
                           m_parent ? m_parent->cloneImpl() : nullptr,
                           m_creation ? m_creation->cloneImpl() : nullptr));
  return copy;
}

std::unique_ptr<V8StackTrace> V8StackTraceImpl::clone() {
  std::vector<Frame> frames;
  for (size_t i = 0; i < m_frames.size(); i++) {
    frames.push_back(m_frames.at(i));
  }
  return std::unique_ptr<V8StackTraceImpl>(new V8StackTraceImpl(
      m_contextGroupId, m_description, frames, nullptr, nullptr));
}

V8StackTraceImpl::V8StackTraceImpl(int contextGroupId,
                                   const String16& description,
                                   std::vector<Frame>& frames,
                                   std::unique_ptr<V8StackTraceImpl> parent,
                                   std::unique_ptr<V8StackTraceImpl> creation)
    : m_contextGroupId(contextGroupId),
      m_description(description),
      m_parent(std::move(parent)),
      m_creation(std::move(creation)) {
  m_frames.swap(frames);
}

V8StackTraceImpl::~V8StackTraceImpl() {}

StringView V8StackTraceImpl::topSourceURL() const {
  DCHECK(m_frames.size());
  return toStringView(m_frames[0].m_scriptName);
}

int V8StackTraceImpl::topLineNumber() const {
  DCHECK(m_frames.size());
  return m_frames[0].m_lineNumber;
}

int V8StackTraceImpl::topColumnNumber() const {
  DCHECK(m_frames.size());
  return m_frames[0].m_columnNumber;
}

StringView V8StackTraceImpl::topFunctionName() const {
  DCHECK(m_frames.size());
  return toStringView(m_frames[0].m_functionName);
}

StringView V8StackTraceImpl::topScriptId() const {
  DCHECK(m_frames.size());
  return toStringView(m_frames[0].m_scriptId);
}

std::unique_ptr<protocol::Runtime::StackTrace>
V8StackTraceImpl::buildInspectorObjectImpl() const {
  return buildInspectorObjectImpl(nullptr);
}

std::unique_ptr<protocol::Runtime::StackTrace>
V8StackTraceImpl::buildInspectorObjectImpl(V8StackTraceImpl* creation) const {
  std::unique_ptr<protocol::Array<protocol::Runtime::CallFrame>> frames =
      protocol::Array<protocol::Runtime::CallFrame>::create();
  for (size_t i = 0; i < m_frames.size(); i++)
    frames->addItem(m_frames.at(i).buildInspectorObject());

  std::unique_ptr<protocol::Runtime::StackTrace> stackTrace =
      protocol::Runtime::StackTrace::create()
          .setCallFrames(std::move(frames))
          .build();
  if (!m_description.isEmpty()) stackTrace->setDescription(m_description);
  if (m_parent) {
    stackTrace->setParent(m_parent->buildInspectorObjectImpl(m_creation.get()));
  }
  if (creation && creation->m_frames.size()) {
    stackTrace->setPromiseCreationFrame(
        creation->m_frames[0].buildInspectorObject());
  }
  return stackTrace;
}

// static
std::unique_ptr<protocol::Runtime::StackTrace>
V8StackTraceImpl::buildInspectorObjectForTail(V8Debugger* debugger) {
  DCHECK(debugger);
  v8::HandleScope handleScope(debugger->inspector()->isolate());
  // Next call collapses possible empty stack and ensures
  // maxAsyncCallChainDepth.
  V8StackTraceImpl* asyncChain = debugger->currentAsyncCallChain();
  if (!asyncChain) return nullptr;
  std::unique_ptr<V8StackTraceImpl> fullChain = V8StackTraceImpl::create(
      debugger, asyncChain->m_contextGroupId, v8::Local<v8::StackTrace>(),
      V8StackTraceImpl::maxCallStackSizeToCapture);
  if (!fullChain || !fullChain->m_parent) return nullptr;
  return fullChain->m_parent->buildInspectorObjectImpl(
      fullChain->m_creation.get());
}

std::unique_ptr<protocol::Runtime::API::StackTrace>
V8StackTraceImpl::buildInspectorObject() const {
  return buildInspectorObjectImpl(nullptr);
}

std::unique_ptr<StringBuffer> V8StackTraceImpl::toString() const {
  String16Builder stackTrace;
  for (size_t i = 0; i < m_frames.size(); ++i) {
    const Frame& frame = m_frames[i];
    stackTrace.append("\n    at " + (frame.functionName().length()
                                         ? frame.functionName()
                                         : "(anonymous function)"));
    stackTrace.append(" (");
    stackTrace.append(frame.sourceURL());
    stackTrace.append(':');
    stackTrace.append(String16::fromInteger(frame.lineNumber()));
    stackTrace.append(':');
    stackTrace.append(String16::fromInteger(frame.columnNumber()));
    stackTrace.append(')');
  }
  String16 string = stackTrace.toString();
  return StringBufferImpl::adopt(string);
}

}  // namespace v8_inspector
