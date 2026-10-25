# UX-R4 控件映射

| 目标 | objectName / 对应责任 |
|---|---|
| 对象与状态 | annotationIdentity、selectedCount |
| 当前段 | annotationSegment、annotationPreviousSegment、annotationNextSegment |
| 几何 / 草稿 | annotationStartMilliseconds、annotationEndMilliseconds、annotationLowerMHz、annotationUpperMHz、editAnnotationRange、cancelAnnotationGeometry |
| 派生物理范围 | annotationRangeSummary |
| 定位 / 创建 | annotationInspectorLocate、annotationInspectorCreateChannel |
| 段管理 | addAnnotationSegment、splitAnnotationSegment、mergeAnnotationSegments、deleteAnnotationSegment |
| 来源与备注 | restoreAnnotationOrigin、adoptAnnotationObservation、editAnnotationNote |
| 技术事实 | annotationEvidenceWidget、annotationEvidenceDetailsToggle、annotationEvidenceDetailsDialog |
| 原始全文 / 复制 | annotationOriginalFieldsToggle、annotationEvidence、annotationCopyEvidence |
| 危险操作 / 撤销 | deleteMarks、undoAnnotations、redoAnnotations |
| 联动 | annotationTable、projectTree、mainPlot、contextAnnotation、auxiliaryDetectionEvidence |
| 唯一设置 / 溢出 | auxChartControls、specChartControls、psdSettings、stftSettings |

既有控件 ID 保留；新增主操作转接既有领域命令，不另建选择或持久化状态。
