# damage.pre 的取消是把伤害改成 0，照常调用原函数

`damage.pre` 被取消时，`DDCore` 把伤害改成 0 后仍然调用 `ApplyHealthDamageInternal`，而不是跳过它。这个函数返回新建的 `Timeline*`，有的调用者不判空直接解引用（`HeroUseItem` 的 `432649`），跳过就会崩溃。伤害为 0 时游戏不扣血、不进死门、不消耗护体层数，界面弹出 0。

`effect.pre` 的取消直接跳过 `EffectApplyInternal`：它返回 void，调用者事先把结果清零了。其余 `.pre` 不支持取消，改字段即可。各主题的取消语义写在 `sdk/include/dd/dd_api.h`。
